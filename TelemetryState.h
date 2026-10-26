#pragma once
#include <cstdint>

// Converted position sample: degrees, meters and horizontal meters per second.
struct TelemetryState {
    double latitude_deg = 0.0;
    double longitude_deg = 0.0;
    double altitude_msl_m = 0.0;
    double relative_alt_m = 0.0;
    double speed_mps = 0.0;
    double heading_deg = 0.0;
};
// Independent store copy; state is meaningful only when has_data is true.
struct Snapshot {
    bool has_data = false;
    TelemetryState state;
    std::uint64_t revision = 0; // Counts accepted updates, including unchanged values.
    bool stale = false;
};
