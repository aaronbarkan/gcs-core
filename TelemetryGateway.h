#pragma once
#include "StateStore.h"
#include <atomic>
#include <thread>
#include <boost/asio.hpp>

class TelemetryGateway {
public:
    explicit TelemetryGateway(StateStore& store);
    ~TelemetryGateway();
    void Start();
    void Stop();
    bool Failed() const { return failed_.load(); }
private:
    void Run();
    StateStore& store_;
    boost::asio::io_context io_;
    boost::asio::ip::udp::socket socket_;
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
    std::thread worker_;
};
