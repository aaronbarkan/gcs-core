#pragma once
#include "StateStore.h"
#include "telemetry.grpc.pb.h"
#include <atomic>

class TelemetryServiceImpl final : public gcs::v1::TelemetryService::Service {
public:
    explicit TelemetryServiceImpl(const StateStore& store) : store_(store) {}
    void Stop() { stopping_ = true; }
    grpc::Status GetTelemetry(grpc::ServerContext* context,
                              const gcs::v1::GetTelemetryRequest* request,
                              gcs::v1::TelemetryEvent* response) override;
    grpc::Status SubscribeTelemetry(grpc::ServerContext* context,
                                    const gcs::v1::SubscribeTelemetryRequest* request,
                                    grpc::ServerWriter<gcs::v1::TelemetryEvent>* writer) override;
private:
    static gcs::v1::TelemetryEvent MakeEvent(const Snapshot& snapshot);
    const StateStore& store_;
    std::atomic<bool> stopping_{false};
};
