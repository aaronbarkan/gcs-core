#pragma once
#include "TelemetryState.h"
#include <common/mavlink.h>

// Returns false for invalid coordinates/heading and leaves result unchanged.
bool ConvertPosition(const mavlink_global_position_int_t& src, TelemetryState& result);
