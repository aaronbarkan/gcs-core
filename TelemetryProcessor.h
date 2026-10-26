#pragma once
#include "TelemetryState.h"
#include <common/mavlink.h>

class TelemetryProcessor {
public:
    bool Convert(const mavlink_global_position_int_t& src, TelemetryState& result) const;
};
