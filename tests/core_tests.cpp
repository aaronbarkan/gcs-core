#include "TelemetryProcessor.h"
#include "StateStore.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>

void Check(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}
int main() {
    try {
        mavlink_global_position_int_t wire{};
        wire.lat = 320000000; wire.lon = 348000000;
        wire.alt = 100000; wire.relative_alt = 50000;
        wire.vx = -300; wire.vy = 400; wire.hdg = 9000;
        TelemetryState value;
        Check(ConvertPosition(wire, value), "valid position rejected");
        Check(value.latitude_deg == 32 && value.longitude_deg == 34.8 &&
              value.altitude_msl_m == 100 && value.relative_alt_m == 50 &&
              value.speed_mps == 5 && value.heading_deg == 90, "incorrect units");
        const auto accepted_latitude = value.latitude_deg;
        wire.lat = 900000001;
        Check(!ConvertPosition(wire, value), "invalid latitude accepted");
        Check(value.latitude_deg == accepted_latitude, "invalid input modified result");
        wire.lat = -900000000; wire.lon = -1800000000; wire.hdg = 35999;
        Check(ConvertPosition(wire, value), "valid boundaries rejected");
        wire.lon = -1800000001;
        Check(!ConvertPosition(wire, value), "invalid longitude accepted");
        wire.lon = 0; wire.hdg = 65535;
        Check(!ConvertPosition(wire, value), "unknown heading accepted");
        wire.hdg = 36000;
        Check(!ConvertPosition(wire, value), "invalid heading accepted");
        StateStore store;
        Check(!store.GetSnapshot().has_data && store.GetSnapshot().revision == 0,
              "initial store not waiting");
        store.Update(value);
        const auto first = store.GetSnapshot();
        Check(first.has_data && !first.stale && first.revision == 1, "first update");
        std::this_thread::sleep_for(std::chrono::milliseconds(2100));
        auto stale = store.GetSnapshot();
        Check(stale.stale && stale.revision == 1 &&
              stale.state.heading_deg == value.heading_deg, "stale must retain value");
        store.Update(value);
        Check(!store.GetSnapshot().stale && store.GetSnapshot().revision == 2, "recovery");
        std::thread writer([&] { for (int i = 0; i < 10000; ++i) store.Update(value); });
        std::uint64_t previous = 2;
        for (int i = 0; i < 10000; ++i) {
            auto snap = store.GetSnapshot();
            Check(snap.revision >= previous && snap.state.heading_deg == value.heading_deg,
                  "inconsistent concurrent snapshot");
            previous = snap.revision;
        }
        writer.join();
        Check(store.GetSnapshot().revision == 10002, "lost updates");
        store.Update(value, std::chrono::steady_clock::now() - std::chrono::seconds(3));
        Check(store.GetSnapshot().stale, "dispatch delay refreshed receipt timestamp");
        std::cout << "Processor and StateStore checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
