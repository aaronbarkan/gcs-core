#include "TelemetryProcessor.h"
#include <cmath>

bool ConvertPosition(const mavlink_global_position_int_t& src, TelemetryState& result) {
    // Unknown heading (65535) and values outside the heading range are skipped.
    if (src.lat < -900000000 || src.lat > 900000000 ||
        src.lon < -1800000000 || src.lon > 1800000000 || src.hdg >= 36000)
        return false;
    result.latitude_deg = src.lat / 1e7;
    result.longitude_deg = src.lon / 1e7;
    result.altitude_msl_m = src.alt / 1000.0;
    result.relative_alt_m = src.relative_alt / 1000.0;
    const double north_mps = src.vx / 100.0;
    const double east_mps = src.vy / 100.0;
    result.speed_mps = std::hypot(north_mps, east_mps);
    result.heading_deg = src.hdg / 100.0;
    return true;
}
