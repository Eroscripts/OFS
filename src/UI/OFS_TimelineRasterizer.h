#pragma once

#include "imgui.h"

#include <cstdint>
#include <memory>
#include <vector>

class Funscript;

// Renders the scrolling script timeline (speed-gradient action lines, points and
// a center playhead) using the editor's own BaseOverlay draw code, so the
// exported strip matches what the user scripts against.
namespace OFS_TimelineRaster
{
    // Draw the timeline strip into an existing draw list within [pos, pos+size].
    // Used both for the live preview (app's ImGui frame) and the offscreen export.
    //   activeIdx   : which script is "active" (lane highlighted when multi-axis).
    //   time        : playhead time in seconds (centered horizontally).
    //   visibleTime : seconds of script spanned across the width.
    //   showHeightLines : draw the 10..90% reference grid behind the actions.
    //   showLabels  : draw each lane's axis name (script title).
    //   allScripts  : stack every enabled script in fixed-height lanes; otherwise
    //                 draw only the active script.
    void DrawInto(ImDrawList* dl, const ImVec2& pos, const ImVec2& size,
                  const std::vector<std::shared_ptr<Funscript>>& scripts,
                  int activeIdx, float time, float visibleTime,
                  bool showHeightLines, bool showLabels, bool allScripts) noexcept;

    // Offscreen variant: renders DrawInto into a top-down, opaque RGBA buffer
    // (w*h*4). Must run on the main thread with the app's GL context current.
    // Returns false on invalid args.
    bool Render(const std::vector<std::shared_ptr<Funscript>>& scripts,
                int activeIdx, float time, float visibleTime,
                int w, int h, bool showHeightLines, bool showLabels, bool allScripts,
                std::vector<uint8_t>& outRGBA) noexcept;

    // Renders `text` centered along the bottom into a transparent, top-down RGBA
    // buffer (w*h*4) using a real TrueType font. For baking readable labels onto
    // exported frames. Main thread + GL context, same as Render.
    bool RenderTextBottomCenter(const char* text, int w, int h,
                                std::vector<uint8_t>& outRGBA) noexcept;
}
