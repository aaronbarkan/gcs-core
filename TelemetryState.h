#pragma once
#include <cstdint>

struct TelemetryState {
    double latitude_deg = 0.0;
    double longitude_deg = 0.0;
    double altitude_msl_m = 0.0;
    double relative_alt_m = 0.0;
    double speed_mps = 0.0;
    double heading_deg = 0.0;
};
struct Snapshot {
    bool has_data = false;
    TelemetryState state;
    std::uint64_t revision = 0;
    bool stale = false;
};
