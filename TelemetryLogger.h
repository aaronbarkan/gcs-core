#pragma once
#include "TelemetrySubscriptions.h"
#include <atomic>
#include <fstream>
#include <string>

class TelemetryGateway;

// Writes RFC 5424 messages on the dispatcher thread; truncates the file on construction.
class TelemetryLogger {
public:
    explicit TelemetryLogger(TelemetryGateway& gateway, const std::string& path = "telemetry.log");
    ~TelemetryLogger();
    TelemetryLogger(const TelemetryLogger&) = delete;
    TelemetryLogger& operator=(const TelemetryLogger&) = delete;

    bool Failed() const { return failed_.load(); }

private:
    void Log(const TelemetryEvent& event);
    std::ofstream file_;
    std::atomic<bool> failed_{false};
    TelemetrySubscriptions::Subscription subscription_;
};
