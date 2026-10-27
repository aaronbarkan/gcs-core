#include "TelemetryGateway.h"
#include <stdexcept>

TelemetryGateway::TelemetryGateway(std::uint16_t port, std::chrono::milliseconds idle)
    : socket_(io_, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), port)), timer_(io_),
      port_(socket_.local_endpoint().port()), sequence_idle_reset_(idle) {
    if (idle.count() <= 0)
        throw std::invalid_argument("sequence reset must be positive");
}
TelemetryGateway::~TelemetryGateway() {
    Stop();
}
void TelemetryGateway::Start() {
    if (events_.IsDispatchThread())
        throw std::logic_error("Start must run on the owner thread");
    if (started_ || stopped_)
        throw std::logic_error("gateway is single-use");
    started_ = true;
    running_ = true;
    try {
        ReceiveNext();
        ScheduleTick();
        worker_ = std::thread([this] {
            try {
                io_.run();
            } catch (const std::exception& error) {
                Fail(error.what());
            } catch (...) {
                Fail("unknown I/O handler exception");
            }
        });
    } catch (...) {
        Close();
        throw;
    }
}
void TelemetryGateway::RequestStop() {
    // Serialize socket/timer cancellation with their completion handlers.
    boost::asio::post(io_, [this] { Close(); });
}
void TelemetryGateway::Stop() {
    if (events_.IsDispatchThread())
        throw std::logic_error("use RequestStop from callbacks");
    if (stopped_)
        return;
    stopped_ = true;
    if (worker_.joinable()) {
        RequestStop();
        worker_.join();
    } else
        Close();
    events_.Stop();
}
void TelemetryGateway::Close() noexcept {
    running_ = false;
    boost::system::error_code ignored;
    socket_.close(ignored);
    try {
        timer_.cancel();
    } catch (...) {
        failed_ = true;
        io_.stop();
    }
}
void TelemetryGateway::Fail(const std::string& error) noexcept {
    failed_ = true;
    Close();
    try {
        TelemetryEvent event;
        event.kind = TelemetryEvent::Kind::ReceiveError;
        event.received_at = std::chrono::steady_clock::now();
        event.error = error;
        events_.Publish(std::move(event));
    } catch (...) { /* Failed() remains observable even if event allocation fails. */
    }
}
void TelemetryGateway::ReceiveNext() {
    socket_.async_receive_from(boost::asio::buffer(buffer_),
                               sender_,
                               [this](const boost::system::error_code& error, std::size_t count) {
                                   if (error == boost::asio::error::operation_aborted)
                                       return;
                                   if (error) {
                                       Fail(error.message());
                                       return;
                                   }
                                   // Publish even zero-length and malformed datagrams before
                                   // parsing.
                                   ProcessDatagram(count, std::chrono::steady_clock::now());
                                   if (running_)
                                       ReceiveNext();
                               });
}
void TelemetryGateway::ProcessDatagram(std::size_t count,
                                       std::chrono::steady_clock::time_point received) {
    TelemetryEvent raw;
    raw.kind = TelemetryEvent::Kind::RawDatagram;
    raw.received_at = received;
    raw.sender_address = sender_.address().to_string();
    raw.sender_port = sender_.port();
    raw.bytes.assign(buffer_.begin(), buffer_.begin() + count);
    events_.Publish(std::move(raw));
    auto& sender = senders_[sender_];
    sender.last_datagram = received;
    mavlink_message_t message{};
    for (std::size_t i = 0; i < count; ++i) {
        // Caller-owned parser state isolates UDP endpoints and gateway instances.
        if (mavlink_frame_char_buffer(
                &sender.parser_message, &sender.parser_status, buffer_[i], &message, nullptr) ==
            MAVLINK_FRAMING_OK)
            AcceptMessage(sender, message, received);
    }
}
void TelemetryGateway::AcceptMessage(Sender& sender,
                                     const mavlink_message_t& message,
                                     std::chrono::steady_clock::time_point received) {
    auto& seq = sender.sources[static_cast<std::uint16_t>(message.sysid) * 256 + message.compid];
    TelemetryEvent event;
    event.received_at = received;
    event.sender_address = sender_.address().to_string();
    event.sender_port = sender_.port();
    event.message = message;
    // Idle resync permits restart recovery; an old replay after this timeout can pass.
    if (received - seq.accepted_at > sequence_idle_reset_)
        seq.seen.reset();
    if (seq.seen[0]) {
        event.expected_sequence = static_cast<std::uint8_t>(seq.last + 1);
        // Modulo 256: 1..127 moves forward; 128 is ambiguous and rejected.
        const auto delta = static_cast<std::uint8_t>(message.seq - seq.last);
        if (delta == 0 || delta >= 128) {
            const auto age = static_cast<std::uint8_t>(seq.last - message.seq);
            event.kind = TelemetryEvent::Kind::SequenceRejected;
            event.rejection = age < 128 && seq.seen[age] ? TelemetryEvent::Rejection::Duplicate
                                                         : TelemetryEvent::Rejection::OutOfOrder;
            events_.Publish(std::move(event));
            return;
        }
        // Accept forward gaps without waiting for lost packets or retransmission.
        event.missing_messages = delta - 1;
        seq.seen <<= delta;
    } else {
        event.expected_sequence = message.seq;
    }
    seq.last = message.seq;
    seq.seen.set(0);
    seq.accepted_at = received;
    event.kind = TelemetryEvent::Kind::Message;
    events_.Publish(std::move(event));
}
void TelemetryGateway::ScheduleTick() {
    // Freshness checks must continue even when no datagrams arrive.
    timer_.expires_after(std::chrono::milliseconds(100));
    timer_.async_wait([this](const boost::system::error_code& error) {
        if (error == boost::asio::error::operation_aborted || !running_)
            return;
        if (error) {
            Fail(error.message());
            return;
        }
        TelemetryEvent tick;
        tick.kind = TelemetryEvent::Kind::Tick;
        tick.received_at = std::chrono::steady_clock::now();
        events_.Publish(tick);
        // Release parser/sequence state for inactive endpoints.
        for (auto it = senders_.begin(); it != senders_.end();) {
            if (tick.received_at - it->second.last_datagram > std::chrono::seconds(30))
                it = senders_.erase(it);
            else
                ++it;
        }
        ScheduleTick();
    });
}
