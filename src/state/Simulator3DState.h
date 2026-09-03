#pragma once

#include "imgui.h"
#include "OFS_StateHandle.h"

// Persisted configuration for the 3D multi-axis simulator.
// App-global (not per-project): camera framing and axis mapping are treated as
// a user preference. Registered in OpenFunscripterState::RegisterAll().
struct Simulator3DState
{
    static constexpr auto StateName = "Simulator3DState";

    // Device preset: 0 = All axes, 1 = SR6 (6-axis), 2 = OSR2+ (stroke+twist+roll+pitch).
    // Controls which axes are driven so the preview matches real hardware.
    int32_t device = 0;

    // Orbit camera. Default is a straight-on front view (yaw/pitch 0), which
    // faces the +Z orientation marker toward the viewer. "Recenter" resets here.
    float camYaw = 0.0f;    // radians
    float camPitch = 0.0f;  // radians
    float camDist = 4.5f;

    // Axis -> motion mapping ranges
    // Ranges match the reference OFS_Simulator3D (OpenFunscripter/OFS_Simulator3D):
    // stroke Lerp(-1,1), surge/sway Lerp(-0.5,0.5), twist +/-120, roll & pitch +/-45.
    float translateRange = 1.0f; // world units (stroke; surge/sway are half this)
    float twistRange = 120.f;    // degrees
    float rollRange = 45.f;      // degrees
    float pitchRange = 45.f;     // degrees

    // Per-axis direction inversion (for matching hardware / convention)
    bool invertStroke = false;
    bool invertSurge = false;
    bool invertSway = false;
    bool invertTwist = false;
    bool invertRoll = false;
    bool invertPitch = false;

    // Display aids
    bool showGrid = true;
    bool showGizmo = true;
    bool showStrokeLine = false; // vertical line from the cylinder bottom to the ground (stroke length)
    bool showHeightText = false;   // draw the stroke position (0..100) under the model
    float heightTextScale = 1.f;   // readout size as a multiple of the UI font size
    bool litMode = false; // false = fast draw-list box; true = GPU lit mesh (render-to-texture)
    bool overlayMode = false; // render in a floating window over the video (front-locked, no orbit)

    // Per-part colors. Roles map to the standard's parts:
    //   shaftColor = stroker, markerColor = twist marker L (red), baseColor = twist marker R (purple)
    ImColor shaftColor = ImColor(0.10f, 0.62f, 0.86f, 1.f);
    ImColor markerColor = ImColor(0.90f, 0.15f, 0.15f, 1.f); // twist L (red)
    ImColor baseColor = ImColor(0.51f, 0.12f, 0.85f, 1.f);   // twist R (purple)

    // Optional extras (like the standard's hidden-by-default tongue).
    bool showTongue = false;
    bool showCenter = false; // optional 3rd (center) orientation indicator
    ImColor tongueColor = ImColor(1.f, 0.38f, 0.38f, 1.f);   // matches standard tongue pink
    ImColor centerColor = ImColor(0.20f, 0.85f, 0.35f, 1.f); // center indicator (green)

    inline static Simulator3DState& State(uint32_t stateHandle) noexcept
    {
        return OFS_AppState<Simulator3DState>(stateHandle).Get();
    }
};

REFL_TYPE(Simulator3DState)
    REFL_FIELD(device)
    REFL_FIELD(camYaw)
    REFL_FIELD(camPitch)
    REFL_FIELD(camDist)
    REFL_FIELD(translateRange)
    REFL_FIELD(twistRange)
    REFL_FIELD(rollRange)
    REFL_FIELD(pitchRange)
    REFL_FIELD(invertStroke)
    REFL_FIELD(invertSurge)
    REFL_FIELD(invertSway)
    REFL_FIELD(invertTwist)
    REFL_FIELD(invertRoll)
    REFL_FIELD(invertPitch)
    REFL_FIELD(showGrid)
    REFL_FIELD(showGizmo)
    REFL_FIELD(showStrokeLine)
    REFL_FIELD(showHeightText)
    REFL_FIELD(heightTextScale)
    REFL_FIELD(litMode)
    REFL_FIELD(overlayMode)
    REFL_FIELD(shaftColor)
    REFL_FIELD(baseColor)
    REFL_FIELD(markerColor)
    REFL_FIELD(showTongue)
    REFL_FIELD(showCenter)
    REFL_FIELD(tongueColor)
    REFL_FIELD(centerColor)
REFL_END
