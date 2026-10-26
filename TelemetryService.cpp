#include "TelemetryService.h"
#include <chrono>
#include <thread>

gcs::v1::TelemetryEvent TelemetryServiceImpl::MakeEvent(const Snapshot& snapshot) {
    gcs::v1::TelemetryEvent event;
    event.set_revision(snapshot.revision);
    event.set_status(!snapshot.has_data ? gcs::v1::WAITING :
                     snapshot.stale ? gcs::v1::STALE : gcs::v1::LIVE);
    if (snapshot.has_data) {
        const auto& value = snapshot.state;
        auto* telemetry = event.mutable_telemetry();
        telemetry->set_latitude_deg(value.latitude_deg);
        telemetry->set_longitude_deg(value.longitude_deg);
        telemetry->set_altitude_msl_m(value.altitude_msl_m);
        telemetry->set_relative_alt_m(value.relative_alt_m);
        telemetry->set_speed_mps(value.speed_mps);
        telemetry->set_heading_deg(value.heading_deg);
    }
    return event;
}
grpc::Status TelemetryServiceImpl::GetTelemetry(
    grpc::ServerContext*, const gcs::v1::GetTelemetryRequest*, gcs::v1::TelemetryEvent* response) {
    const auto snapshot = store_.GetSnapshot();
    if (!snapshot.has_data)
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "No valid GLOBAL_POSITION_INT received yet");
    *response = MakeEvent(snapshot);
    return grpc::Status::OK;
}
grpc::Status TelemetryServiceImpl::SubscribeTelemetry(
    grpc::ServerContext* context, const gcs::v1::SubscribeTelemetryRequest*,
    grpc::ServerWriter<gcs::v1::TelemetryEvent>* writer) {
    // Each RPC has its own cursor; slow clients may skip intermediate revisions.
    bool first = true;
    std::uint64_t last_revision = 0;
    auto last_status = gcs::v1::WAITING;
    while (!stopping_ && !context->IsCancelled()) {
        // GetSnapshot releases the store lock before any network I/O or sleep.
        const auto event = MakeEvent(store_.GetSnapshot());
        if (first || event.revision() != last_revision || event.status() != last_status) {
            if (!writer->Write(event)) break;
            first = false;
            last_revision = event.revision();
            last_status = event.status();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return grpc::Status::OK;
}
