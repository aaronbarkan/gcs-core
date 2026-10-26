#include "TelemetryStateSubscriber.h"
#include "TelemetryGateway.h"
#include "TelemetryProcessor.h"
#include <iomanip>
#include <iostream>
#include <sstream>

TelemetryStateSubscriber::TelemetryStateSubscriber(TelemetryGateway& gateway, StateStore& store)
    : store_(store) {
    subscription_ = gateway.Subscribe(
        [this](TelemetrySubscriptions::Event event) { HandleTelemetryEvent(*event); });
}

TelemetryStateSubscriber::~TelemetryStateSubscriber() {
    // Wait for an active callback before any of the state it accesses is destroyed.
    subscription_.Reset();
}

void TelemetryStateSubscriber::LogTelemetry(const TelemetryState& value, std::uint64_t revision) {
    std::ostringstream line;
    line << "TELEMETRY rev=" << revision << std::fixed << std::setprecision(7)
         << " lat=" << value.latitude_deg << " lon=" << value.longitude_deg << std::setprecision(2)
         << " alt_msl_m=" << value.altitude_msl_m << " rel_alt_m=" << value.relative_alt_m
         << " speed_mps=" << value.speed_mps << " hdg_deg=" << value.heading_deg;
    std::cout << line.str() << std::endl;
}

void TelemetryStateSubscriber::HandlePositionMessage(const TelemetryEvent& event) {
    const auto& message = event.message;
    if (message.sysid != 1 || message.compid != 1 ||
        message.msgid != MAVLINK_MSG_ID_GLOBAL_POSITION_INT)
        return;
    mavlink_global_position_int_t position{};
    mavlink_msg_global_position_int_decode(&message, &position);
    TelemetryState value;
    if (!ConvertPosition(position, value))
        return;
    store_.Update(value, event.received_at);
    const auto snapshot = store_.GetSnapshot();
    LogTelemetry(value, snapshot.revision);
    // A queued sample may already be stale by the time this callback runs.
    if (!was_live_ && !snapshot.stale) {
        std::cout << "LINK LIVE" << std::endl;
        was_live_ = true;
    }
}

// Runs serially on the dispatcher; raw and sequence events need no action here.
void TelemetryStateSubscriber::HandleTelemetryEvent(const TelemetryEvent& event) {
    if (event.kind == TelemetryEvent::Kind::ReceiveError) {
        std::cout << "ERROR UDP receiver: " << event.error << std::endl;
        return;
    }
    if (event.kind == TelemetryEvent::Kind::Tick) {
        if (was_live_ && store_.GetSnapshot().stale) {
            std::cout << "LINK STALE - no position received for >2 seconds" << std::endl;
            was_live_ = false;
        }
        return;
    }
    if (event.kind == TelemetryEvent::Kind::Message)
        HandlePositionMessage(event);
}
