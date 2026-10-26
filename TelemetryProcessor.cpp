#include "TelemetryProcessor.h"
#include <cmath>

bool TelemetryProcessor::Convert(const mavlink_global_position_int_t& src,
                                 TelemetryState& result) const {
    // Unknown heading (65535) and values outside the heading range are skipped.
    if (src.lat < -900000000 || src.lat > 900000000 ||
        src.lon < -1800000000 || src.lon > 1800000000 || src.hdg >= 36000)
        return false;
    TelemetryState value;
    value.latitude_deg = src.lat / 1e7;
    value.longitude_deg = src.lon / 1e7;
    value.altitude_msl_m = src.alt / 1000.0;
    value.relative_alt_m = src.relative_alt / 1000.0;
    const double north_mps = src.vx / 100.0;
    const double east_mps = src.vy / 100.0;
    value.speed_mps = std::sqrt(north_mps * north_mps + east_mps * east_mps);
    value.heading_deg = src.hdg / 100.0;
    result = value;
    return true;
}
