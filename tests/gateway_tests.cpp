#include "TelemetryGateway.h"
#include <condition_variable>
#include <future>
#include <iostream>
#include <stdexcept>

using Kind = TelemetryEvent::Kind;
using Reject = TelemetryEvent::Rejection;
using Event = TelemetrySubscriptions::Event;
using Bytes = std::vector<std::uint8_t>;
using namespace std::chrono;

void Check(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}
struct Inbox {
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<Event> events;
    void Add(Event event) {
        { std::lock_guard<std::mutex> lock(mutex); events.push_back(std::move(event)); }
        ready.notify_all();
    }
    std::vector<Event> Wait(Kind kind, std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex);
        Check(ready.wait_for(lock, seconds(3), [&] {
            std::size_t n = 0;
            for (const auto& e : events) if (e->kind == kind) ++n;
            return n >= count;
        }), "event delivery timed out");
        std::vector<Event> result;
        for (const auto& e : events) if (e->kind == kind) result.push_back(e);
        return result;
    }
};
Bytes Frame(std::uint8_t seq, std::uint8_t sys = 1, std::uint8_t comp = 1, bool position = false) {
    mavlink_status_t status{};
    status.current_tx_seq = seq;
    mavlink_message_t message{};
    if (position)
        mavlink_msg_global_position_int_pack_status(sys, comp, &status, &message,
            0, 320000000, 348000000, 100000, 50000, 0, 500, 0, 9000);
    else mavlink_msg_heartbeat_pack_status(sys, comp, &status, &message, 2, 0, 128, 0, 4);
    Bytes bytes(MAVLINK_MAX_PACKET_LEN);
    bytes.resize(mavlink_msg_to_send_buffer(bytes.data(), &message));
    return bytes;
}
struct Sender {
    boost::asio::io_context io;
    boost::asio::ip::udp::socket socket{io, {boost::asio::ip::udp::v4(), 0}};
    void Send(const Bytes& bytes, std::uint16_t port) {
        socket.send_to(boost::asio::buffer(bytes),
            {boost::asio::ip::address_v4::loopback(), port});
    }
};

void OrderingAndRaw() {
    Inbox inbox;
    TelemetryGateway gateway(0);
    auto sub = gateway.Subscribe([&](Event e) { inbox.Add(e); });
    gateway.Start();
    Sender a, b, c;
    auto first = Frame(100);
    a.Send(first, gateway.Port());
    a.Send(Frame(100), gateway.Port()); // duplicate
    a.Send(Frame(102, 1, 1, true), gateway.Port()); // one lost message
    a.Send(Frame(101), gateway.Port()); // late gap filler
    a.Send(Frame(100), gateway.Port()); // old duplicate in recent history
    a.Send(Frame(10, 2), gateway.Port()); // separate system
    a.Send(Frame(10, 1, 2), gateway.Port()); // separate component
    b.Send(Frame(100), gateway.Port()); // separate UDP endpoint
    for (int seq : {254, 255, 0, 1}) c.Send(Frame(seq), gateway.Port());
    c.Send(Frame(255), gateway.Port()); // duplicate across wrap
    c.Send(Frame(253), gateway.Port()); // out of order across wrap
    c.Send(Frame(129), gateway.Port()); // exactly half-range: ambiguous, reject
    auto corrupt = Frame(2);
    corrupt.back() ^= 0x80;
    c.Send(corrupt, gateway.Port());
    c.Send(Bytes{1, 2, 3}, gateway.Port());
    c.Send(Bytes{}, gateway.Port()); // zero-byte datagram still recorded
    c.Send(Frame(2), gateway.Port()); // corrupt input didn't advance the sequence
    auto accepted = inbox.Wait(Kind::Message, 10);
    auto rejected = inbox.Wait(Kind::SequenceRejected, 6);
    auto raw = inbox.Wait(Kind::RawDatagram, 19);
    Check(accepted.size() == 10 && rejected.size() == 6 && raw.size() == 19, "event counts");
    Check(raw.front()->bytes == first && raw[15]->bytes == corrupt && raw[17]->bytes.empty(), "raw ownership/content");
    Check(accepted[1]->missing_messages == 1 && accepted[1]->expected_sequence == 101, "gap accounting");
    Check(accepted[1]->message.msgid == MAVLINK_MSG_ID_GLOBAL_POSITION_INT, "position delivery");
    Check(rejected[0]->rejection == Reject::Duplicate && rejected[1]->rejection == Reject::OutOfOrder &&
          rejected[2]->rejection == Reject::Duplicate, "ordering reasons");
    Check(rejected[3]->rejection == Reject::Duplicate && rejected[4]->rejection == Reject::OutOfOrder &&
          rejected[5]->rejection == Reject::OutOfOrder, "wrap rejection reasons");
    Check(accepted.back()->message.seq == 2 && accepted.back()->missing_messages == 0, "CRC recovery");
    gateway.Stop();
    // Every message or rejection must follow a raw event, including same-datagram frames.
    Event preceding_raw;
    for (const auto& e : inbox.events) {
        if (e->kind == Kind::RawDatagram) preceding_raw = e;
        if (e->kind == Kind::Message || e->kind == Kind::SequenceRejected)
            Check(preceding_raw && preceding_raw->received_at == e->received_at &&
                  preceding_raw->sender_port == e->sender_port &&
                  preceding_raw->sender_address == e->sender_address,
                  "message/rejection did not follow its raw datagram");
    }
    Check(!gateway.Failed(), "gateway failed");
}

void ParserIsolationAndReset() {
    Inbox inbox, other_inbox;
    TelemetryGateway gateway(0, milliseconds(150)), other(0);
    auto sub = gateway.Subscribe([&](Event e) { inbox.Add(e); });
    auto other_sub = other.Subscribe([&](Event e) { other_inbox.Add(e); });
    gateway.Start(); other.Start();
    Sender a, b;
    auto packet = Frame(200);
    a.Send(Bytes(packet.begin(), packet.begin() + 12), gateway.Port());
    b.Send(Frame(40), gateway.Port());
    a.Send(Frame(7), other.Port()); // same sender, separate gateway/parser
    a.Send(Bytes(packet.begin() + 12, packet.end()), gateway.Port());
    auto combined = Frame(201);
    auto next = Frame(202);
    combined.insert(combined.end(), next.begin(), next.end());
    a.Send(combined, gateway.Port());
    auto messages = inbox.Wait(Kind::Message, 4);
    Check(messages[0]->message.seq == 40 && messages[1]->message.seq == 200 &&
          messages[3]->message.seq == 202, "split/coalesced frames or sender isolation");
    Check(other_inbox.Wait(Kind::Message, 1)[0]->message.seq == 7, "gateway isolation");
    // Tick independently wakes an otherwise idle receiver; enough time to resync.
    inbox.Wait(Kind::Tick, 3);
    a.Send(Frame(0), gateway.Port());
    const auto reset = inbox.Wait(Kind::Message, 5).back();
    Check(reset->message.seq == 0 && reset->expected_sequence == 0 &&
          reset->missing_messages == 0, "idle sequence reset");
    auto start = steady_clock::now();
    gateway.Stop(); gateway.Stop(); other.Stop();
    Check(steady_clock::now() - start < seconds(1), "idle shutdown blocked");
    bool threw = false;
    try { gateway.Start(); } catch (const std::logic_error&) { threw = true; }
    Check(threw, "single-use lifecycle not enforced");
    TelemetryGateway never_started(0);
    never_started.Stop(); never_started.Stop();
}

void Observers() {
    Inbox inbox;
    TelemetryGateway gateway(0);
    auto throwing = gateway.Subscribe([](Event) { throw std::runtime_error("subscriber failure"); });
    TelemetrySubscriptions::Subscription self;
    unsigned self_calls = 0;
    self = gateway.Subscribe([&](Event) { ++self_calls; self.Reset(); });
    unsigned mutable_calls = 0;
    auto stateful = gateway.Subscribe([count = 0u, &mutable_calls](Event) mutable {
        mutable_calls = ++count;
    });
    auto good = gateway.Subscribe([&](Event e) { inbox.Add(e); });
    gateway.Start();
    Sender sender;
    sender.Send(Frame(1), gateway.Port());
    inbox.Wait(Kind::Message, 1);
    stateful.Reset();
    Check(mutable_calls >= 2, "mutable callback state was copied between events");
    Check(gateway.CallbackErrors() >= 2, "callback exception not isolated/reported");
    good.Reset();
    sender.Send(Frame(2), gateway.Port());
    gateway.Stop();
    Check(self_calls == 1, "self unsubscribe failed");
    Check(inbox.Wait(Kind::Message, 1).size() == 1, "callback ran after unsubscribe");
    // Tokens can safely outlive their publisher.
    throwing.Reset();
}

void SlowObserverAndConcurrentReset() {
    Inbox inbox;
    TelemetryGateway gateway(0);
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    auto blocked = gateway.Subscribe([&](Event e) {
        if (e->kind == Kind::RawDatagram) { entered.set_value(); released.wait(); }
    });
    auto good = gateway.Subscribe([&](Event e) { inbox.Add(e); });
    gateway.Start();
    Sender sender;
    sender.Send(Frame(1), gateway.Port());
    Check(entered.get_future().wait_for(seconds(2)) == std::future_status::ready, "slow callback not entered");
    auto resetting = std::async(std::launch::async, [&] { blocked.Reset(); });
    sender.Send(Frame(2), gateway.Port());
    const bool waited = resetting.wait_for(milliseconds(200)) == std::future_status::timeout;
    const auto before_release = steady_clock::now();
    release.set_value();
    resetting.get();
    Check(waited, "unsubscribe failed to wait for active callback");
    auto raw = inbox.Wait(Kind::RawDatagram, 2);
    Check(raw[1]->received_at < before_release, "subscriber blocked the Asio receiver");
    std::promise<void> stop_requested;
    auto requesting = gateway.Subscribe([&](Event e) {
        if (e->kind == Kind::Tick) {
            gateway.RequestStop();
            stop_requested.set_value();
        }
    });
    Check(stop_requested.get_future().wait_for(seconds(2)) == std::future_status::ready,
          "callback stop request did not run");
    gateway.Stop();
}
void QueueOverflow() {
    TelemetrySubscriptions subscriptions;
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    unsigned delivered = 0;
    auto sub = subscriptions.Subscribe([&](Event) {
        if (++delivered == 1) {
            entered.set_value();
            released.wait_for(seconds(3));
        }
    });
    subscriptions.Publish(TelemetryEvent{});
    const bool started = entered.get_future().wait_for(seconds(2)) == std::future_status::ready;
    unsigned queued = 0;
    bool overflow = false;
    try {
        for (; queued < 1025; ++queued) subscriptions.Publish(TelemetryEvent{});
    } catch (const std::overflow_error&) { overflow = true; }
    release.set_value();
    subscriptions.Stop();
    Check(started && overflow && queued == 1024, "dispatch queue capacity not enforced");
    Check(delivered == queued + 1, "shutdown lost queued deliveries after overflow");
}

void GatewayOverflow() {
    TelemetryGateway gateway(0);
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    auto sub = gateway.Subscribe([&, first = true](Event e) mutable {
        if (first && e->kind == Kind::RawDatagram) {
            first = false;
            entered.set_value();
            released.wait_for(seconds(3));
        }
    });
    gateway.Start();
    Sender sender;
    sender.Send(Frame(0), gateway.Port());
    const bool started = entered.get_future().wait_for(seconds(2)) == std::future_status::ready;
    Bytes batch;
    for (unsigned i = 1; i <= 200; ++i) {
        const auto frame = Frame(static_cast<std::uint8_t>(i));
        batch.insert(batch.end(), frame.begin(), frame.end());
    }
    for (unsigned i = 0; i < 6; ++i) sender.Send(batch, gateway.Port());
    const auto deadline = steady_clock::now() + seconds(2);
    while (!gateway.Failed() && steady_clock::now() < deadline)
        std::this_thread::sleep_for(milliseconds(1));
    const bool failed = gateway.Failed();
    release.set_value();
    gateway.Stop();
    Check(started && failed, "queue overflow did not fail the gateway");
}

int main() {
    try {
        std::cout << "Ordering and raw datagrams" << std::endl;
        OrderingAndRaw();
        std::cout << "Parser isolation and reset" << std::endl;
        ParserIsolationAndReset();
        std::cout << "Observer lifetime" << std::endl;
        Observers();
        std::cout << "Slow observer and concurrent reset" << std::endl;
        SlowObserverAndConcurrentReset();
        std::cout << "Queue capacity and gateway overload" << std::endl;
        QueueOverflow();
        GatewayOverflow();
        std::cout << "Async gateway, sequence, raw delivery, observer lifetime and shutdown checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
