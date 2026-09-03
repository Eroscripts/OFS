#pragma once

#include "OFS_StateHandle.h"

#include <cstdint>
#include <vector>

// One stop of the action-line speed gradient. color is RGBA packed (ImU32).
struct LineColorStop
{
    float pos = 0.f;           // 0..1 along the speed axis
    uint32_t color = 0xFFFFFFFFu;
};

// Persisted choice of the timeline action-line color scheme.
struct LineColorState
{
    static constexpr auto StateName = "LineColorState";

    int32_t profile = 0; // 0 = Fork, 1 = Classic (OFS), 2 = Custom
    std::vector<LineColorStop> customStops; // used when profile == Custom

    inline static LineColorState& State(uint32_t stateHandle) noexcept
    {
        return OFS_AppState<LineColorState>(stateHandle).Get();
    }
};

REFL_TYPE(LineColorStop)
    REFL_FIELD(pos)
    REFL_FIELD(color)
REFL_END

REFL_TYPE(LineColorState)
    REFL_FIELD(profile)
    REFL_FIELD(customStops)
REFL_END
