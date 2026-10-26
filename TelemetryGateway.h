#pragma once
#include "TelemetrySubscriptions.h"
#include <array>
#include <atomic>
#include <bitset>
#include <map>
#include <thread>
#include <boost/asio.hpp>

// Async UDP transport: publish raw bytes first, then validated MAVLink events.
// Parsing/timers run on one Asio thread; consumers run on a separate dispatcher.
class TelemetryGateway {
public:
    // Binds immediately; port 0 requests an OS-selected port (useful in tests).
    explicit TelemetryGateway(std::uint16_t port = 14550,
        std::chrono::milliseconds sequence_idle_reset = std::chrono::seconds(5));
    ~TelemetryGateway();
    // Keep the returned handle alive to keep receiving events.
    TelemetrySubscriptions::Subscription Subscribe(TelemetrySubscriptions::Callback callback) {
        return events_.Subscribe(std::move(callback));
    }
    // Single-use lifecycle. Start/Stop belong to the owner, not callbacks.
    void Start();
    // Idempotent: cancel I/O, join reception, then drain callbacks that must return.
    void Stop();
    // Safe from callbacks; the owner must still call Stop to join threads.
    void RequestStop();
    bool Failed() const { return failed_.load(); }
    std::uint16_t Port() const { return port_; }
    std::uint64_t CallbackErrors() const { return events_.CallbackErrors(); }
private:
    struct Sequence {
        std::uint8_t last = 0;
        std::bitset<128> seen; // Bit n records acceptance n sequence steps behind last.
        std::chrono::steady_clock::time_point accepted_at;
    };
    struct Sender {
        mavlink_message_t parser_message{};
        mavlink_status_t parser_status{};
        std::map<std::uint16_t, Sequence> sources; // Key: system ID * 256 + component ID.
        std::chrono::steady_clock::time_point last_datagram;
    };
    void ReceiveNext();
    void ScheduleTick();
    void ProcessDatagram(std::size_t count, std::chrono::steady_clock::time_point received);
    void AcceptMessage(Sender& sender, const mavlink_message_t& message,
                       std::chrono::steady_clock::time_point received);
    void Close() noexcept;
    void Fail(const std::string& error) noexcept;
    boost::asio::io_context io_;
    boost::asio::ip::udp::socket socket_;
    boost::asio::steady_timer timer_;
    // Async receive arguments must outlive the outstanding operation.
    boost::asio::ip::udp::endpoint sender_;
    std::array<std::uint8_t, 65536> buffer_{};
    std::map<boost::asio::ip::udp::endpoint, Sender> senders_;
    const std::uint16_t port_;
    const std::chrono::milliseconds sequence_idle_reset_;
    TelemetrySubscriptions events_;
    bool started_ = false;
    bool stopped_ = false;
    bool running_ = false; // Accessed on io_ thread once started.
    std::atomic<bool> failed_{false};
    std::thread worker_;
};
