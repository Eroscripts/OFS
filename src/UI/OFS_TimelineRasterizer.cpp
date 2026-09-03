#include "OFS_TimelineRasterizer.h"

#include "Funscript.h"
#include "ScriptPositionsOverlayMode.h"

#include "OFS_ImGui.h"
#include "OFS_Shader.h"
#include "OFS_GL.h"
#include "OFS_Util.h"

#include "imgui.h"
#include "imgui_impl/imgui_impl_opengl3.h"

#include <algorithm>
#include <iterator>

void OFS_TimelineRaster::DrawInto(ImDrawList* dl, const ImVec2& pos, const ImVec2& size,
                                  const std::vector<std::shared_ptr<Funscript>>& scripts,
                                  int activeIdx, float time, float visibleTime,
                                  bool showHeightLines, bool showLabels, bool allScripts) noexcept
{
    if (size.x < 2.f || size.y < 2.f || visibleTime <= 0.f) return;

    // Which scripts to draw: all enabled, or just the active one.
    std::vector<int> idxs;
    if (allScripts) {
        for (int i = 0; i < (int)scripts.size(); ++i)
            if (scripts[i] && scripts[i]->Enabled) idxs.push_back(i);
    }
    if (idxs.empty()) {
        if (activeIdx >= 0 && activeIdx < (int)scripts.size() && scripts[activeIdx]) idxs.push_back(activeIdx);
    }
    if (idxs.empty()) return;

    const int n = (int)idxs.size();
    constexpr float spacing = 2.f; // gap between lanes
    const float laneH = (size.y - spacing * (n - 1)) / (float)n;
    if (laneH < 2.f) return;

    const float offsetTime = time - (visibleTime / 2.f);

    // The strip always shows action lines + points; drive the editor's global
    // flags on and restore them afterwards so the interactive timeline is unaffected.
    const bool savedLines = BaseOverlay::ShowLines;
    const bool savedPoints = BaseOverlay::ShowPoints;
    const float savedPointSize = BaseOverlay::PointSize;
    BaseOverlay::ShowLines = true;
    BaseOverlay::ShowPoints = true;

    for (int li = 0; li < n; ++li) {
        const int si = idxs[li];
        auto script = scripts[si].get();
        if (!script) continue;

        const float laneY = pos.y + (float)li * (laneH + spacing);
        const ImVec2 laneMin(pos.x, laneY);
        const ImVec2 laneMax(pos.x + size.x, laneY + laneH);

        // Vertical inset so pos 0/100 point dots (and line ends) aren't clipped
        // at the lane edges. MaxPointSize is 8.
        const float margin = Util::Min(10.f, laneH * 0.25f);

        // Lane background (matches the editor's inactive-script gradient).
        dl->AddRectFilledMultiColor(laneMin, laneMax,
            IM_COL32(0, 0, 50, 255), IM_COL32(0, 0, 50, 255),
            IM_COL32(0, 0, 20, 255), IM_COL32(0, 0, 20, 255));
        dl->PushClipRect(laneMin, laneMax, true);

        OverlayDrawingCtx ctx = {};
        ctx.scripts = &scripts;
        ctx.drawingScriptIdx = si;
        ctx.activeScriptIdx = activeIdx;
        ctx.hoveredScriptIdx = -1;
        ctx.drawnScriptCount = n;
        ctx.drawList = dl;
        ctx.canvasPos = ImVec2(laneMin.x, laneMin.y + margin);
        ctx.canvasSize = ImVec2(size.x, laneH - 2.f * margin);
        ctx.visibleTime = visibleTime;
        ctx.offsetTime = offsetTime;
        ctx.totalDuration = time + visibleTime; // unused by the calls below, just nonzero
        ctx.selectionFromIdx = ctx.selectionToIdx = 0; // no selection highlight in export

        auto& actions = script->Actions();
        auto s = actions.lower_bound(FunscriptAction(offsetTime, 0));
        if (s != actions.begin()) s -= 1;
        auto e = actions.lower_bound(FunscriptAction(offsetTime + visibleTime, 0));
        if (e != actions.end()) e += 1;
        ctx.actionFromIdx = (int32_t)std::distance(actions.begin(), s);
        ctx.actionToIdx = (int32_t)std::distance(actions.begin(), e);

        if (showHeightLines) BaseOverlay::DrawHeightLines(ctx); // behind the actions
        BaseOverlay::DrawActionLines(ctx);
        BaseOverlay::DrawActionPoints(ctx);

        // Axis label (script title) so multi-axis strips are legible. Use an
        // explicit size (the offscreen font is loaded large) proportional to the lane.
        const auto& title = script->Title();
        if (showLabels && !title.empty()) {
            ImFont* lf = ImGui::GetFont();
            const float labelPx = Util::Clamp(laneH * 0.30f, 9.f, 20.f);
            const ImVec2 tp(laneMin.x + 4.f, laneMin.y + 2.f);
            dl->AddText(lf, labelPx, ImVec2(tp.x + 1.f, tp.y + 1.f), IM_COL32(0, 0, 0, 220), title.c_str());
            dl->AddText(lf, labelPx, tp, IM_COL32(255, 255, 255, 255), title.c_str());
        }

        // Lane border; highlight the active lane when several are stacked.
        const uint32_t border = (si == activeIdx && n > 1) ? IM_COL32(0, 180, 0, 255)
                                                           : IM_COL32(120, 120, 120, 255);
        dl->AddRect(laneMin, laneMax, border);
        dl->PopClipRect();
    }

    // Center playhead across the whole strip.
    dl->AddLine(ImVec2(pos.x + size.x * 0.5f, pos.y), ImVec2(pos.x + size.x * 0.5f, pos.y + size.y),
                IM_COL32(255, 255, 255, 255), 2.f);

    BaseOverlay::ShowLines = savedLines;
    BaseOverlay::ShowPoints = savedPoints;
    BaseOverlay::PointSize = savedPointSize;
}

// A dedicated ImGui context + framebuffer kept alive across frames so a whole
// export job doesn't pay the cost of rebuilding the font atlas each frame.
// Created lazily on first use (main thread, GL current) and reused thereafter.
namespace
{
    ImGuiContext* g_ctx = nullptr;
    ImFont* g_font = nullptr;      // RobotoMono, for baked text labels
    constexpr float kFontLoadPx = 64.f;
    GLuint g_fbo = 0, g_colorTex = 0;
    int g_w = 0, g_h = 0;
    GLuint g_txFbo = 0, g_txTex = 0; // separate FBO for text overlays
    int g_txW = 0, g_txH = 0;

    void ensureContext() noexcept
    {
        if (g_ctx) return;
        auto prev = ImGui::GetCurrentContext();
        g_ctx = ImGui::CreateContext(); // own font atlas / draw-list shared data
        ImGui::SetCurrentContext(g_ctx);
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().LogFilename = nullptr;
        ImGui::StyleColorsDark();
        // Load a real font for baked labels; falls back to the built-in font.
        const auto fontPath = Util::Resource("fonts/RobotoMono-Regular.ttf");
        g_font = ImGui::GetIO().Fonts->AddFontFromFileTTF(fontPath.c_str(), kFontLoadPx);
        if (!g_font) g_font = ImGui::GetIO().Fonts->AddFontDefault();
        ImGui_ImplOpenGL3_Init(OFS_SHADER_VERSION);
        ImGui::SetCurrentContext(prev);
    }

    // Create/resize a framebuffer with an RGBA8 color texture. Returns success.
    bool ensureFboImpl(GLuint& fbo, GLuint& tex, int& cw, int& ch, int w, int h) noexcept
    {
        if (fbo && w == cw && h == ch) return true;
        if (!fbo) glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        if (!tex) glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, OFS_InternalTexFormat, w, h, 0, OFS_TexFormat, GL_UNSIGNED_BYTE, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        GLenum bufs[1] = { GL_COLOR_ATTACHMENT0 };
        glDrawBuffers(1, bufs);
        const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (ok) { cw = w; ch = h; }
        return ok;
    }

    inline bool ensureFbo(int w, int h) noexcept { return ensureFboImpl(g_fbo, g_colorTex, g_w, g_h, w, h); }
    inline bool ensureTextFbo(int w, int h) noexcept { return ensureFboImpl(g_txFbo, g_txTex, g_txW, g_txH, w, h); }

    // Flip an RGBA buffer vertically in place (glReadPixels is bottom-up).
    void flipY(std::vector<uint8_t>& buf, int w, int h) noexcept
    {
        const size_t stride = (size_t)w * 4u;
        for (int y = 0; y < h / 2; ++y) {
            uint8_t* a = buf.data() + (size_t)y * stride;
            uint8_t* b = buf.data() + (size_t)(h - 1 - y) * stride;
            std::swap_ranges(a, a + stride, b);
        }
    }
}

bool OFS_TimelineRaster::Render(const std::vector<std::shared_ptr<Funscript>>& scripts,
                                int activeIdx, float time, float visibleTime,
                                int w, int h, bool showHeightLines, bool showLabels, bool allScripts,
                                std::vector<uint8_t>& outRGBA) noexcept
{
    if (w < 1 || h < 1 || visibleTime <= 0.f) return false;

    ensureContext();
    if (!ensureFbo(w, h)) return false;

    // Save caller GL/ImGui state we touch.
    GLint prevFbo = 0, prevViewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    auto prevCtx = ImGui::GetCurrentContext();

    ImGui::SetCurrentContext(g_ctx);
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DisplayFramebufferScale = ImVec2(1.f, 1.f);
    io.DeltaTime = 1.f / 60.f;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    ImDrawList* dl = ImGui::GetForegroundDrawList(ImGui::GetMainViewport());
    DrawInto(dl, ImVec2(0.f, 0.f), ImVec2((float)w, (float)h), scripts, activeIdx, time, visibleTime,
             showHeightLines, showLabels, allScripts);

    // Render the draw list into our offscreen framebuffer.
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glViewport(0, 0, w, h);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui::Render();
    OFS_ImGui::CurrentlyRenderedViewport = ImGui::GetMainViewport();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    OFS_ImGui::CurrentlyRenderedViewport = nullptr;

    outRGBA.resize((size_t)w * (size_t)h * 4u);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, outRGBA.data());

    // Restore caller state.
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    ImGui::SetCurrentContext(prevCtx);

    flipY(outRGBA, w, h); // glReadPixels is bottom-up
    return true;
}

bool OFS_TimelineRaster::RenderTextBottomCenter(const char* text, int w, int h,
                                                std::vector<uint8_t>& outRGBA) noexcept
{
    if (w < 1 || h < 1 || !text || !*text) return false;

    ensureContext();
    if (!ensureTextFbo(w, h)) return false;

    GLint prevFbo = 0, prevViewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    auto prevCtx = ImGui::GetCurrentContext();

    ImGui::SetCurrentContext(g_ctx);
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DisplayFramebufferScale = ImVec2(1.f, 1.f);
    io.DeltaTime = 1.f / 60.f;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    ImDrawList* dl = ImGui::GetForegroundDrawList(ImGui::GetMainViewport());
    ImFont* font = g_font ? g_font : ImGui::GetFont();
    // Size the text to the buffer height; scaling down from kFontLoadPx stays crisp.
    const float fontPx = Util::Clamp((float)h * 0.14f, 12.f, kFontLoadPx);
    const ImVec2 ts = font->CalcTextSizeA(fontPx, 1e9f, 0.f, text);
    const ImVec2 tp((w - ts.x) * 0.5f, (float)h - ts.y - (float)h * 0.03f);
    dl->AddText(font, fontPx, ImVec2(tp.x + 2.f, tp.y + 2.f), IM_COL32(0, 0, 0, 210), text); // shadow
    dl->AddText(font, fontPx, tp, IM_COL32(255, 255, 255, 255), text);

    glBindFramebuffer(GL_FRAMEBUFFER, g_txFbo);
    glViewport(0, 0, w, h);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui::Render();
    OFS_ImGui::CurrentlyRenderedViewport = ImGui::GetMainViewport();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    OFS_ImGui::CurrentlyRenderedViewport = nullptr;

    outRGBA.resize((size_t)w * (size_t)h * 4u);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, outRGBA.data());

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    ImGui::SetCurrentContext(prevCtx);

    flipY(outRGBA, w, h);
    return true;
}
