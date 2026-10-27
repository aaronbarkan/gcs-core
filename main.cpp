#include "TelemetryGateway.h"
#include "TelemetryLogger.h"
#include "TelemetryService.h"
#include "TelemetryStateSubscriber.h"
#include <grpcpp/grpcpp.h>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

namespace {
// Signal handlers only set a flag; main performs cleanup outside signal context.
volatile std::sig_atomic_t stop_requested = 0;
void HandleSignal(int) { stop_requested = 1; }
}
int main() {
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
    try {
        StateStore store;
        TelemetryGateway gateway;
        TelemetryLogger logger(gateway);
        TelemetryStateSubscriber telemetry(gateway, store);
        TelemetryServiceImpl service(store);
        grpc::ServerBuilder builder;
        builder.AddListeningPort("0.0.0.0:50051", grpc::InsecureServerCredentials());
        builder.RegisterService(&service);
        auto server = builder.BuildAndStart();
        if (!server) throw std::runtime_error("cannot bind gRPC port 50051");
        std::cout << "GCS Core listening UDP=0.0.0.0:14550 gRPC=0.0.0.0:50051" << std::endl;
        std::cout << "LINK WAITING - no valid GLOBAL_POSITION_INT yet" << std::endl;
        gateway.Start();
        // Supervise shutdown only; reception and callbacks run on their own threads.
        while (!stop_requested && !gateway.Failed() && !logger.Failed())
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // Stop streams, drain gateway callbacks, then cancel remaining RPCs.
        service.Stop();
        gateway.Stop();
        server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(1));
        server->Wait();
        std::cout << (gateway.Failed() || logger.Failed() ? "GCS Core stopped: telemetry service failed"
                                      : "GCS Core stopped") << std::endl;
        return gateway.Failed() || logger.Failed() ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "GCS Core failed: " << error.what() << std::endl;
        return 1;
    }
}
