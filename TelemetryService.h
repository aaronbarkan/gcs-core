#pragma once
#include "StateStore.h"
#include "telemetry.grpc.pb.h"
#include <atomic>

// Synchronous gRPC facade over the shared store; the store must outlive the service.
class TelemetryServiceImpl final : public gcs::v1::TelemetryService::Service {
public:
    explicit TelemetryServiceImpl(const StateStore& store) : store_(store) {}
    // Signals streaming loops; Server::Shutdown supplies the deadline for blocked writes.
    void Stop() { stopping_ = true; }
    // NOT_FOUND before the first sample; stale snapshots still return successfully.
    grpc::Status GetTelemetry(grpc::ServerContext* context,
                              const gcs::v1::GetTelemetryRequest* request,
                              gcs::v1::TelemetryEvent* response) override;
    // Sends the initial snapshot, then changed revision/status at 200 ms intervals.
    grpc::Status SubscribeTelemetry(grpc::ServerContext* context,
                                    const gcs::v1::SubscribeTelemetryRequest* request,
                                    grpc::ServerWriter<gcs::v1::TelemetryEvent>* writer) override;
private:
    static gcs::v1::TelemetryEvent MakeEvent(const Snapshot& snapshot);
    const StateStore& store_;
    std::atomic<bool> stopping_{false};
};
