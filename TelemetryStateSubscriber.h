#pragma once
#include "StateStore.h"
#include "TelemetrySubscriptions.h"

class TelemetryGateway;

// Applies accepted positions to the store and preserves the existing stdout display.
// The store must outlive this subscriber. Destroy on the owner thread, not a callback.
class TelemetryStateSubscriber {
public:
    // Subscribes after member initialization; destruction waits for any active callback.
    TelemetryStateSubscriber(TelemetryGateway& gateway, StateStore& store);
    ~TelemetryStateSubscriber();
    // The callback captures this, so the subscriber must keep a stable address.
    TelemetryStateSubscriber(const TelemetryStateSubscriber&) = delete;
    TelemetryStateSubscriber& operator=(const TelemetryStateSubscriber&) = delete;
    TelemetryStateSubscriber(TelemetryStateSubscriber&&) = delete;
    TelemetryStateSubscriber& operator=(TelemetryStateSubscriber&&) = delete;

private:
    static void LogTelemetry(const TelemetryState& value, std::uint64_t revision);
    void HandlePositionMessage(const TelemetryEvent& event);
    void HandleTelemetryEvent(const TelemetryEvent& event);

    StateStore& store_;
    bool was_live_ = false; // Dispatcher-only log transition flag, not authoritative status.
    TelemetrySubscriptions::Subscription subscription_;
};
