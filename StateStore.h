#pragma once
#include "TelemetryState.h"
#include <chrono>
#include <mutex>

class StateStore {
public:
    void Update(const TelemetryState& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_.state = value;
        latest_.has_data = true;
        ++latest_.revision;
        last_receive_ = std::chrono::steady_clock::now();
    }
    Snapshot GetSnapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        Snapshot result = latest_;
        result.stale = result.has_data &&
            std::chrono::steady_clock::now() - last_receive_ > std::chrono::seconds(2);
        return result;
    }
private:
    mutable std::mutex mutex_;
    Snapshot latest_;
    std::chrono::steady_clock::time_point last_receive_;
};
