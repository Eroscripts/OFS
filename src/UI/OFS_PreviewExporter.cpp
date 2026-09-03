#include "OFS_PreviewExporter.h"
#include "state/PreviewExportState.h"
#include "state/states/ChapterState.h"

#include "OpenFunscripter.h"
#include "OFS_Simulator3D.h"
#include "OFS_TimelineRasterizer.h"
#include "OFS_Project.h"
#include "Funscript.h"
#include "OFS_Util.h"

#include "subprocess.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "state/SimulatorState.h"
#include "stb_sprintf.h"
#include "stb_image_write.h"
#include "OFS_FileLogging.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <thread>
#include <vector>

// Even output width for a target height at the video's aspect (16:9 fallback). 
static int widthForHeight(int h, uint16_t vw, uint16_t vh) noexcept
{
    int w = (vh > 0) ? (int)std::lround((double)vw * h / (double)vh) : (h * 16 / 9);
    w -= (w & 1);
    return w < 2 ? 2 : w;
}

// Stroke position (0..1) of the active script at time t; centered if it has no data.
static float strokeAt(OpenFunscripter* app, float t) noexcept
{
    auto& s = app->ActiveFunscript();
    if (!s || s->Actions().empty()) return 0.5f;
    return s->GetPositionAtTime(t) / 100.f;
}

// The 2D ScriptSimulator's persisted config (colors/opacity), shared by the app.
static SimulatorState& sim2DConfig() noexcept
{
    static const uint32_t handle = OFS_ProjectState<SimulatorState>::Register(SimulatorState::StateName);
    return SimulatorState::State(handle);
}

// Pack an ImColor to ImU32 with the sim's global opacity folded into alpha.
static ImU32 simColor(const ImColor& col, float opacity) noexcept
{
    ImVec4 v = col.Value;
    v.w *= opacity;
    return ImGui::ColorConvertFloat4ToU32(v);
}

// Previous/next scripted action positions (0..100, -1 if none) around time t.
static void adjacentPos(std::shared_ptr<Funscript>& s, float t, int& prevPos, int& nextPos) noexcept
{
    prevPos = -1; nextPos = -1;
    if (!s) return;
    const FunscriptAction* pa = s->GetActionAtTime(t, 0.02f);
    if (!pa) pa = s->GetPreviousActionBehind(t);
    const FunscriptAction* na = s->GetNextActionAhead(t);
    if (pa && na == pa) na = s->GetNextActionAhead(pa->atS);
    if (pa) prevPos = pa->pos;
    if (na) nextPos = na->pos;
}

// CPU-rasterize the 2D stroke bar into a top-down, transparent RGBA buffer, using
// the ScriptSimulator's configured colors + height lines + indicators.
static void renderBar2D(float pos01, int prevPos, int nextPos, int w, int h,
                        const SimulatorState& sc, std::vector<uint8_t>& out) noexcept
{
    out.assign((size_t)w * (size_t)h * 4u, 0);
    auto fill = [&](int x0, int y0, int x1, int y1, ImU32 c) {
        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
        if (x1 > w) x1 = w; if (y1 > h) y1 = h;
        const uint8_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, a = (c >> 24) & 0xFF;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                const size_t i = ((size_t)y * w + x) * 4u;
                out[i] = r; out[i + 1] = g; out[i + 2] = b; out[i + 3] = a;
            }
    };
    const int barW = (int)(w * 0.42f);
    const int bx0 = (w - barW) / 2, bx1 = bx0 + barW;
    const int m = (int)(h * 0.04f);
    const int by0 = m, by1 = h - m, barH = by1 - by0;
    if (barW <= 0 || barH <= 0) return;
    const float op = sc.GlobalOpacity;
    auto hline = [&](float posPct, ImU32 c, int lw) {
        if (lw < 1) lw = 1;
        const int y = by1 - (int)((posPct / 100.f) * barH);
        fill(bx0, y - lw / 2, bx1, y - lw / 2 + lw, c);
    };
    fill(bx0, by0, bx1, by1, simColor(sc.Back, op));                        // track
    fill(bx0, by1 - (int)(pos01 * barH), bx1, by1, simColor(sc.Front, op)); // fill
    if (sc.EnableHeightLines) {
        const ImU32 lc = simColor(sc.ExtraLines, op);
        for (int i = 1; i < 10; ++i) hline(i * 10.f, lc, (int)sc.LineWidth);
    }
    if (sc.EnableIndicators) {
        const ImU32 ic = simColor(sc.Indicator, op);
        if (prevPos > 0 && prevPos < 100) hline((float)prevPos, ic, (int)sc.LineWidth);
        if (nextPos > 0 && nextPos < 100) hline((float)nextPos, ic, (int)sc.LineWidth);
    }
    const int bt = 2;                                                       // border
    const ImU32 bc = simColor(sc.Border, op);
    fill(bx0, by0, bx1, by0 + bt, bc);
    fill(bx0, by1 - bt, bx1, by1, bc);
    fill(bx0, by0, bx0 + bt, by1, bc);
    fill(bx1 - bt, by0, bx1, by1, bc);
}

// Alpha-composite a straight-alpha RGBA overlay (same size) over a destination
// RGBA buffer, in place. Used to lay baked text onto a rendered sim frame.
static void compositeOver(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src, int w, int h) noexcept
{
    const size_t n = (size_t)w * (size_t)h;
    if (dst.size() < n * 4 || src.size() < n * 4) return;
    for (size_t i = 0; i < n; ++i) {
        const size_t p = i * 4u;
        const uint8_t sa = src[p + 3];
        if (sa == 0) continue;
        if (sa == 255) {
            dst[p] = src[p]; dst[p + 1] = src[p + 1]; dst[p + 2] = src[p + 2]; dst[p + 3] = 255;
            continue;
        }
        const float a = sa / 255.f, ia = 1.f - a;
        for (int c = 0; c < 3; ++c)
            dst[p + c] = (uint8_t)(src[p + c] * a + dst[p + c] * ia + 0.5f);
        dst[p + 3] = (uint8_t)(sa + dst[p + 3] * ia + 0.5f);
    }
}

void OFS_PreviewExporter::Init() noexcept
{
    stateHandle = OFS_AppState<PreviewExportState>::Register(PreviewExportState::StateName);
}

void OFS_PreviewExporter::spawnFfmpeg(std::vector<std::string> args, std::vector<std::string> tempDirs) noexcept
{
    { // log the exact command so failures are diagnosable from the log file
        std::string cmd;
        for (auto& s : args) { cmd += '"'; cmd += s; cmd += "\" "; }
        LOGF_INFO("[PreviewExport] %s", cmd.c_str());
    }
    lastErrorText.clear();
    exporting.store(true);
    hasRun = true;
    std::thread([this, args = std::move(args), tempDirs = std::move(tempDirs)]() mutable {
        std::vector<const char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& s : args) argv.push_back(s.c_str());
        argv.push_back(nullptr);

        int rc = -1;
        std::string err;
        struct subprocess_s proc;
        if (subprocess_create(argv.data(), subprocess_option_no_window, &proc) == 0) {
            if (proc.stdout_file) { fclose(proc.stdout_file); proc.stdout_file = nullptr; }
            subprocess_join(&proc, &rc);
            // Read ffmpeg's stderr (kept small by -loglevel error) for diagnostics.
            if (proc.stderr_file) {
                char b[512];
                size_t n;
                while ((n = fread(b, 1, sizeof(b), proc.stderr_file)) > 0)
                    err.append(b, n);
            }
            subprocess_destroy(&proc);
        }
        for (auto& d : tempDirs) {
            if (d.empty()) continue;
            std::error_code ec;
            std::filesystem::remove_all(std::filesystem::u8path(d), ec);
        }
        lastErrorText = std::move(err);
        lastResult.store(rc);
        if (rc != 0) LOGF_ERROR("[PreviewExport] ffmpeg failed (%d): %s", rc, lastErrorText.c_str());
        exporting.store(false);
    }).detach();
}

void OFS_PreviewExporter::OpenForChapter(float start, float end) noexcept
{
    auto& st = PreviewExportState::State(stateHandle);
    st.rangeMode = 1; // timestamps
    startTime = start;
    endTime = end;
    startEditing = false;
    endEditing = false;
}

void OFS_PreviewExporter::startExport(const std::string& outputPath, const std::vector<Segment>& segments) noexcept
{
    if (exporting.load() || renderingFrames) return;
    if (segments.empty()) return;
    for (auto& s : segments) if (s.dur <= 0.f) return;
    auto& st = PreviewExportState::State(stateHandle);
    auto app = OpenFunscripter::ptr;
    const char* videoPath = app->player->VideoPath();
    if (!videoPath || !*videoPath) return;

    const int N = (int)segments.size();
    const bool compilation = N > 1;   // several slices concatenated

    const int outH = st.resolution; // height of the video region
    const uint16_t vw = app->player->VideoWidth();
    const uint16_t vh = app->player->VideoHeight();
    // With 16:9 padding the video region becomes a fixed landscape frame; the
    // source is scaled to fit and centered inside it with black bars.
    const int outW = st.padTo169 ? (((int)std::lround(outH * 16.0 / 9.0)) & ~1)
                                 : widthForHeight(outH, vw, vh);

    const bool haveScript = !app->LoadedFunscripts().empty();
    const bool doSim = st.overlaySim && haveScript;
    const bool doTimeline = st.overlayTimeline && haveScript;
    const bool needFrames = doSim || doTimeline;
    const bool useFC = needFrames || st.padTo169 || compilation; // needs -filter_complex
    const bool wantAudio = (st.format == 1) && app->player->HasAudio(); // AV1 carries audio

    // Timeline strip band height (extends the canvas below the video region).
    int bandH = 0;
    if (doTimeline) { bandH = ((int)std::lround(st.timelineHeightFrac * outH)) & ~1; if (bandH < 2) bandH = 2; }
    const int totalH = outH + bandH;

    // Sim overlay rect, positioned against the (padded) video region.
    int ox = 0, oy = 0, ow = 0, oh = 0;
    if (doSim) {
        ow = ((int)std::lround(st.simW * outW)) & ~1; if (ow < 2) ow = 2;
        oh = ((int)std::lround(st.simH * outH)) & ~1; if (oh < 2) oh = 2;
        ox = (int)std::lround(st.simX * outW);
        oy = (int)std::lround(st.simY * outH);
    }

    // Set up one PNG-sequence layer per enabled overlay stream (sim first, then
    // timeline) so the ffmpeg input indices below line up.
    std::vector<ExportLayer> layers;
    auto addLayer = [&](LayerKind kind, int w, int h, const char* dirName, const char* prefix) {
        ExportLayer L;
        L.kind = kind; L.w = w; L.h = h; L.filePrefix = prefix;
        L.tempDir = Util::Prefpath(dirName);
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::u8path(L.tempDir), ec);
        Util::CreateDirectories(std::filesystem::u8path(L.tempDir));
        layers.push_back(std::move(L));
    };
    if (doSim) addLayer(st.sim2D ? LayerKind::Sim2D : LayerKind::Sim3D, ow, oh, "preview_frames", "frame_");
    if (doTimeline) addLayer(LayerKind::Timeline, outW, bandH, "preview_tl", "tl_");

    char buf[256];

    // Use OFS's ffmpeg (now the gyan "full" build, which includes libsvtav1).
    const std::string ffmpeg = Util::FfmpegPath().u8string();
    std::vector<std::string> a;
    a.push_back(ffmpeg);
    a.push_back("-y");
    a.push_back("-hide_banner");
    a.push_back("-loglevel"); a.push_back("error");
    // One fast-seeked video input per segment. -ss and -t must both be INPUT options
    // (before -i); a -t after -i would bind to a following input instead of trimming.
    for (auto& s : segments) {
        stbsp_snprintf(buf, sizeof(buf), "%.3f", s.start); a.push_back("-ss"); a.push_back(buf);
        stbsp_snprintf(buf, sizeof(buf), "%.3f", s.dur);   a.push_back("-t");  a.push_back(buf);
        a.push_back("-i"); a.push_back(videoPath);
    }

    if (useFC) {
        // Add a framerate-tagged image-sequence input for each overlay layer (after
        // the N video inputs, so overlay input indices start at N).
        int simInput = -1, tlInput = -1, nextInput = N;
        for (auto& L : layers) {
            stbsp_snprintf(buf, sizeof(buf), "%d", st.fps); a.push_back("-framerate"); a.push_back(buf);
            a.push_back("-i");
            a.push_back((std::filesystem::u8path(L.tempDir) / (L.filePrefix + "%05d.png")).u8string());
            if (L.kind == LayerKind::Timeline) tlInput = nextInput; else simInput = nextInput;
            ++nextInput;
        }

        // Concatenate the segment videos first (compilation), then fps/scale/pad.
        std::string fg;
        std::string base = "[0:v]";
        if (compilation) {
            for (int i = 0; i < N; ++i) { stbsp_snprintf(buf, sizeof(buf), "[%d:v]", i); fg += buf; }
            stbsp_snprintf(buf, sizeof(buf), "concat=n=%d:v=1[cat];", N); fg += buf;
            base = "[cat]";
        }
        fg += base;
        fg += "fps=" + std::to_string(st.fps) + ",";
        if (st.padTo169) {
            stbsp_snprintf(buf, sizeof(buf),
                "scale=%d:%d:force_original_aspect_ratio=decrease,pad=%d:%d:(ow-iw)/2:(oh-ih)/2:black",
                outW, outH, outW, outH);
        } else {
            stbsp_snprintf(buf, sizeof(buf), "scale=%d:%d", outW, outH);
        }
        fg += buf;
        if (bandH > 0) { stbsp_snprintf(buf, sizeof(buf), ",pad=%d:%d:0:0:black", outW, totalH); fg += buf; }
        fg += "[base]";
        std::string last = "[base]";
        if (doSim) {
            stbsp_snprintf(buf, sizeof(buf), ";%s[%d:v]overlay=%d:%d[withsim]", last.c_str(), simInput, ox, oy);
            fg += buf; last = "[withsim]";
        }
        if (doTimeline) {
            stbsp_snprintf(buf, sizeof(buf), ";%s[%d:v]overlay=0:%d[outv]", last.c_str(), tlInput, outH);
            fg += buf; last = "[outv]";
        }
        // Audio (AV1 only): concat each segment's audio for a compilation, else map source.
        std::string audioLabel;
        if (wantAudio && compilation) {
            fg += ";";
            for (int i = 0; i < N; ++i) { stbsp_snprintf(buf, sizeof(buf), "[%d:a]", i); fg += buf; }
            stbsp_snprintf(buf, sizeof(buf), "concat=n=%d:v=0:a=1[outa]", N); fg += buf;
            audioLabel = "[outa]";
        }
        a.push_back("-filter_complex"); a.push_back(fg);
        // filter_complex disables auto stream selection, so map explicitly.
        a.push_back("-map"); a.push_back(last);
        if (wantAudio) {
            a.push_back("-map");
            a.push_back(compilation ? audioLabel : std::string("0:a?"));
        }
    } else {
        stbsp_snprintf(buf, sizeof(buf), "fps=%d", st.fps);
        a.push_back("-filter:v"); a.push_back(buf);
        stbsp_snprintf(buf, sizeof(buf), "%dx%d", outW, outH);
        a.push_back("-s"); a.push_back(buf);
    }

    if (st.format == 0) { // animated WebP (lossless)
        a.push_back("-vcodec"); a.push_back("libwebp");
        a.push_back("-lossless"); a.push_back("1");
        a.push_back("-loop"); a.push_back("0");
        a.push_back("-preset"); a.push_back("default");
    } else { // AV1 (mp4) via libsvtav1 (from PATH ffmpeg)
        const int preset = Util::Clamp(st.av1Preset, 0, 13);
        a.push_back("-c:v"); a.push_back("libsvtav1");
        a.push_back("-crf"); a.push_back("30");
        a.push_back("-b:v"); a.push_back("0");
        stbsp_snprintf(buf, sizeof(buf), "%d", preset);
        a.push_back("-preset"); a.push_back(buf);
    }
    if (st.format == 1) { // AV1/mp4 carries the source audio; WebP cannot
        a.push_back("-c:a"); a.push_back("aac");
        a.push_back("-b:a"); a.push_back("192k");
    } else {
        a.push_back("-an");
    }
    a.push_back(outputPath);

    if (needFrames) {
        // Kick off the incremental frame-render job; ffmpeg spawns when it finishes.
        // Segments share one duration, so frames-per-segment is uniform.
        int framesPerSeg = (int)std::lround((double)segments[0].dur * st.fps);
        if (framesPerSeg < 1) framesPerSeg = 1;
        jobFrame = 0;
        jobFramesPerSeg = framesPerSeg;
        jobFrameCount = framesPerSeg * N;
        jobFps = (float)st.fps;
        jobSegStarts.clear();
        for (auto& s : segments) jobSegStarts.push_back(s.start);
        jobLayers = std::move(layers);
        jobArgs = std::move(a);
        renderingFrames = true;
        exporting.store(true); // UI shows busy through the whole job
        hasRun = true;
    } else {
        spawnFfmpeg(std::move(a), {}); // no PNG sequences (e.g. plain 16:9 pad)
    }
}

void OFS_PreviewExporter::Update() noexcept
{
    if (!renderingFrames) return;
    auto app = OpenFunscripter::ptr;
    auto& sim = app->GetSimulator3D();
    auto& scripts = app->LoadedFunscripts();
    const auto& st = PreviewExportState::State(stateHandle);
    const int activeIdx = (int)app->LoadedProject->ActiveIdx();
    const SimulatorState* sc2D = &sim2DConfig();

    // Render a small batch per UI frame to keep the app responsive.
    constexpr int kBatch = 4;
    std::vector<uint8_t> textOverlay; // reused scratch for baked height text
    for (int k = 0; k < kBatch && jobFrame < jobFrameCount; ++k, ++jobFrame) {
        // Output frame -> source time within its segment (segments are concatenated).
        int seg = jobFramesPerSeg > 0 ? (jobFrame / jobFramesPerSeg) : 0;
        if (seg >= (int)jobSegStarts.size()) seg = (int)jobSegStarts.size() - 1;
        const int localFrame = jobFrame - seg * jobFramesPerSeg;
        const float t = jobSegStarts[seg] + (float)localFrame / jobFps;
        char name[48];
        for (auto& L : jobLayers) {
            bool ok = true;
            switch (L.kind) {
            case LayerKind::Sim2D: {
                int pp, np;
                adjacentPos(app->ActiveFunscript(), t, pp, np);
                renderBar2D(strokeAt(app, t), pp, np, L.w, L.h, *sc2D, L.pixels);
                break;
            }
            case LayerKind::Sim3D:
                ok = sim.RenderPoseRGBA(scripts, t, L.w, L.h, L.pixels);
                break;
            case LayerKind::Timeline:
                ok = OFS_TimelineRaster::Render(scripts, activeIdx, t, st.timelineVisibleTime,
                                                L.w, L.h, st.timelineShowHeightLines,
                                                st.timelineShowLabels, st.timelineAllScripts, L.pixels);
                break;
            }
            if (ok && st.simShowHeightText && (L.kind == LayerKind::Sim2D || L.kind == LayerKind::Sim3D)) {
                char hb[8];
                stbsp_snprintf(hb, sizeof(hb), "%d", (int)std::lround(strokeAt(app, t) * 100.f));
                if (OFS_TimelineRaster::RenderTextBottomCenter(hb, L.w, L.h, textOverlay))
                    compositeOver(L.pixels, textOverlay, L.w, L.h);
            }
            if (ok) {
                stbsp_snprintf(name, sizeof(name), "%s%05d.png", L.filePrefix.c_str(), jobFrame);
                const auto p = (std::filesystem::u8path(L.tempDir) / name).u8string();
                stbi_write_png(p.c_str(), L.w, L.h, 4, L.pixels.data(), L.w * 4);
            }
        }
    }

    if (jobFrame >= jobFrameCount) {
        renderingFrames = false;
        std::vector<std::string> dirs;
        for (auto& L : jobLayers) dirs.push_back(L.tempDir);
        spawnFfmpeg(std::move(jobArgs), std::move(dirs));
        jobArgs.clear();
        jobLayers.clear();
    }
}

void OFS_PreviewExporter::ShowWindow(bool* open) noexcept
{
    if (!*open) return;
    auto& st = PreviewExportState::State(stateHandle);
    auto app = OpenFunscripter::ptr;
    const uint16_t vw = app->player->VideoWidth();
    const uint16_t vh = app->player->VideoHeight();

    ImGui::Begin("Animated Preview Export###PREVIEW_EXPORT", open, ImGuiWindowFlags_None);

    ImGui::Combo("Format", &st.format, "Animated WebP\0AV1 (mp4)\0\0");

    // Resolution: standard heights <= native, at the video's aspect.
    {
        char curLabel[32];
        stbsp_snprintf(curLabel, sizeof(curLabel), "%dx%d", widthForHeight(st.resolution, vw, vh), st.resolution);
        if (ImGui::BeginCombo("Resolution", curLabel)) {
            // 90 gives 160x90 for 16:9 sources (thumbnails); 180 -> 320x180.
            static const int heights[] = {2160, 1440, 1080, 720, 540, 480, 360, 240, 180, 90};
            for (int h : heights) {
                if (vh > 0 && h > (int)vh) continue; // no upscaling
                char lbl[32];
                stbsp_snprintf(lbl, sizeof(lbl), "%dx%d", widthForHeight(h, vw, vh), h);
                if (ImGui::Selectable(lbl, st.resolution == h)) st.resolution = h;
            }
            ImGui::EndCombo();
        }
    }

    {
        ImGui::Checkbox("Pad video to 16:9 (black bars)", &st.padTo169);
        if (vw > 0 && vh > vw) {
            ImGui::SameLine();
            ImGui::TextDisabled("(recommended for vertical video)");
        }
    }

    if (ImGui::InputInt("FPS", &st.fps)) st.fps = Util::Clamp(st.fps, 1, 60);

    if (st.format == 0) {
        ImGui::TextDisabled("WebP is exported lossless.");
    } else {
        if (ImGui::SliderInt("AV1 speed/preset", &st.av1Preset, 0, 13))
            st.av1Preset = Util::Clamp(st.av1Preset, 0, 13);
        ImGui::SameLine();
        ImGui::TextDisabled("(0 best \xE2\x80\xA2 13 fast)");
    }

    ImGui::Separator();
    ImGui::TextDisabled("Range (use the chapter right-click menu to fill this from a chapter)");

    const double duration = app->player->Duration();
    // HH:MM:SS.mmm text fields, matching the timeline format.
    auto timeField = [&](const char* label, float& seconds, std::string& str, bool& editing) {
        if (!editing) {
            char b[32];
            Util::FormatTime(b, sizeof(b), seconds, true);
            str = b;
        }
        ImGui::InputText(label, &str);
        editing = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            bool ok = false;
            const float v = Util::ParseTime(str.c_str(), &ok);
            if (ok) seconds = Util::Clamp(v, 0.f, (float)duration);
        }
    };
    timeField("Start (h:m:s.ms)", startTime, startStr, startEditing);
    timeField("End (h:m:s.ms)", endTime, endStr, endEditing);
    const float rStart = startTime;
    const float rEnd = endTime;

    // ---- Compilation: several slices spread evenly across the whole video ----
    ImGui::Separator();
    ImGui::Checkbox("Compilation (slices across the whole video)", &st.compilation);
    if (st.compilation) {
        if (ImGui::SliderInt("Slices", &st.compSlices, 2, 30))
            st.compSlices = Util::Clamp(st.compSlices, 1, 100);
        if (ImGui::SliderFloat("Slice duration", &st.compSliceDur, 0.5f, 10.f, "%.1f s"))
            st.compSliceDur = Util::Clamp(st.compSliceDur, 0.1f, 60.f);
        ImGui::Text("Output length: %.1f s  (%d \xC3\x97 %.1f s)",
                    st.compSlices * st.compSliceDur, st.compSlices, st.compSliceDur);
        if ((float)duration > 0.f && st.compSlices * st.compSliceDur > (float)duration)
            ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "Slices exceed the video length \xE2\x80\x94 they will overlap.");
        ImGui::TextDisabled("Ignores the Start/End range above.");
    }
    const bool haveRange = st.compilation ? (duration > 0.0) : (rEnd > rStart);

    // ---- Overlay the simulator on the video (draggable live preview) ----
    ImGui::Separator();
    ImGui::Checkbox("Overlay simulator on video", &st.overlaySim);
    if (st.overlaySim) {
        ImGui::SameLine();
        ImGui::Checkbox("2D bar (single axis)", &st.sim2D);
        ImGui::Checkbox("Show height value", &st.simShowHeightText);
        const uint32_t frameTex = app->player->FrameTexture();
        if (frameTex && vw > 0 && vh > 0) {
            ImGui::TextDisabled(st.padTo169
                ? "16:9 preview \xE2\x80\xA2 drag the sim into the black bars \xE2\x80\xA2 corner to resize"
                : "Drag to move \xE2\x80\xA2 drag corner to resize");
            float previewW = ImGui::GetContentRegionAvail().x;
            if (previewW > 480.f) previewW = 480.f;
            // When padding to 16:9 the drag canvas is the padded frame (so overlays can
            // be positioned in the bars); otherwise it's the video at its native aspect.
            const float previewH = st.padTo169 ? (previewW * 9.f / 16.f)
                                               : (previewW * (float)vh / (float)vw);
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            auto* dl = ImGui::GetWindowDrawList();
            if (st.padTo169) {
                // Black 16:9 canvas with the source letterboxed/pillarboxed inside, matching
                // the exported frame so the overlay lands where the preview shows it.
                dl->AddRectFilled(origin, ImVec2(origin.x + previewW, origin.y + previewH), IM_COL32(0, 0, 0, 255));
                float vidW = previewW, vidH = previewW * (float)vh / (float)vw;
                if (vidH > previewH) { vidH = previewH; vidW = previewH * (float)vw / (float)vh; }
                const ImVec2 vMin(origin.x + (previewW - vidW) * 0.5f, origin.y + (previewH - vidH) * 0.5f);
                dl->AddImage((ImTextureID)(intptr_t)frameTex, vMin, ImVec2(vMin.x + vidW, vMin.y + vidH));
                ImGui::Dummy(ImVec2(previewW, previewH));
            } else {
                ImGui::Image((ImTextureID)(intptr_t)frameTex, ImVec2(previewW, previewH));
            }

            const ImVec2 bMin(origin.x + st.simX * previewW, origin.y + st.simY * previewH);
            const int bw = (int)(st.simW * previewW);
            const int bh = (int)(st.simH * previewH);
            const ImVec2 bMax(bMin.x + bw, bMin.y + bh);

            if (bw > 4 && bh > 4 && !app->LoadedFunscripts().empty()) {
                if (st.sim2D) {
                    // 2D stroke bar matching the ScriptSimulator (colors + height lines + indicators).
                    auto& sc = sim2DConfig();
                    const float op = sc.GlobalOpacity;
                    const float ct = (float)app->player->CurrentTime();
                    const float pos01 = strokeAt(app, ct);
                    const float trackW = (bMax.x - bMin.x) * 0.42f;
                    const float tx0 = bMin.x + ((bMax.x - bMin.x) - trackW) * 0.5f, tx1 = tx0 + trackW;
                    const float mrg = (bMax.y - bMin.y) * 0.04f;
                    const float ty0 = bMin.y + mrg, ty1 = bMax.y - mrg, barH = ty1 - ty0;
                    dl->AddRectFilled(ImVec2(tx0, ty0), ImVec2(tx1, ty1), simColor(sc.Back, op));
                    dl->AddRectFilled(ImVec2(tx0, ty1 - pos01 * barH), ImVec2(tx1, ty1), simColor(sc.Front, op));
                    if (sc.EnableHeightLines) {
                        const ImU32 lc = simColor(sc.ExtraLines, op);
                        for (int i = 1; i < 10; ++i) {
                            const float y = ty1 - (i * 0.1f) * barH;
                            dl->AddLine(ImVec2(tx0, y), ImVec2(tx1, y), lc, sc.LineWidth);
                        }
                    }
                    if (sc.EnableIndicators) {
                        int pp, np;
                        adjacentPos(app->ActiveFunscript(), ct, pp, np);
                        const ImU32 ic = simColor(sc.Indicator, op);
                        const ImU32 tc = simColor(sc.Text, op);
                        auto ind = [&](int pos) {
                            if (pos <= 0 || pos >= 100) return;
                            const float y = ty1 - (pos * 0.01f) * barH;
                            dl->AddLine(ImVec2(tx0, y), ImVec2(tx1, y), ic, sc.LineWidth);
                            char b[8]; stbsp_snprintf(b, sizeof(b), "%d", pos);
                            dl->AddText(ImVec2((tx0 + tx1) * 0.5f - 7.f, y - 7.f), tc, b);
                        };
                        ind(pp); ind(np);
                    }
                    dl->AddRect(ImVec2(tx0, ty0), ImVec2(tx1, ty1), simColor(sc.Border, op), 0.f, 0, 2.f);
                } else {
                    const uint32_t simTex = app->GetSimulator3D().RenderPoseTexture(
                        app->LoadedFunscripts(), (float)app->player->CurrentTime(), bw, bh);
                    dl->AddImage((ImTextureID)(intptr_t)simTex, bMin, bMax, ImVec2(0, 1), ImVec2(1, 0));
                }
                if (st.simShowHeightText) {
                    char b[8];
                    stbsp_snprintf(b, sizeof(b), "%d",
                        (int)std::lround(strokeAt(app, (float)app->player->CurrentTime()) * 100.f));
                    const ImVec2 ts = ImGui::CalcTextSize(b);
                    const ImVec2 tp((bMin.x + bMax.x) * 0.5f - ts.x * 0.5f, bMax.y - ts.y - 3.f);
                    dl->AddText(ImVec2(tp.x + 1.f, tp.y + 1.f), IM_COL32(0, 0, 0, 255), b);
                    dl->AddText(tp, IM_COL32(255, 255, 255, 255), b);
                }
            }
            const float grip = 22.f; // easy-to-grab resize corner
            dl->AddRect(bMin, bMax, IM_COL32(0xFF, 0xE0, 0x40, 0xD0), 0.f, 0, 2.f);
            dl->AddTriangleFilled(ImVec2(bMax.x - grip, bMax.y), bMax, ImVec2(bMax.x, bMax.y - grip),
                                  IM_COL32(0xFF, 0xE0, 0x40, 0xF0));

            // One handle over the whole box; the drag's starting position (corner or
            // not) decides resize vs move. Avoids unreliable overlapping-button clicks.
            ImGui::SetCursorScreenPos(bMin);
            ImGui::InvisibleButton("##simBox", ImVec2((float)bw, (float)bh));
            const ImVec2 mp = ImGui::GetIO().MousePos;
            const bool inCorner = (mp.x >= bMax.x - grip && mp.y >= bMax.y - grip);
            if (ImGui::IsItemActivated())
                draggingResize = inCorner;
            if ((ImGui::IsItemHovered() && inCorner) || (draggingResize && ImGui::IsItemActive()))
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
            if (ImGui::IsItemActive()) {
                const ImVec2 d = ImGui::GetIO().MouseDelta;
                if (draggingResize) {
                    st.simW += d.x / previewW;
                    st.simH += d.y / previewH;
                } else {
                    st.simX += d.x / previewW;
                    st.simY += d.y / previewH;
                }
            }

            st.simW = Util::Clamp(st.simW, 0.05f, 1.f);
            st.simH = Util::Clamp(st.simH, 0.05f, 1.f);
            st.simX = Util::Clamp(st.simX, 0.f, 1.f - st.simW);
            st.simY = Util::Clamp(st.simY, 0.f, 1.f - st.simH);

            ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + previewH + 4.f));
        } else {
            ImGui::TextDisabled("Load a video to position the overlay.");
        }
        if (app->LoadedFunscripts().empty())
            ImGui::TextDisabled("No script loaded \xE2\x80\x94 nothing to composite.");
    }

    // ---- Append the scrolling script timeline as a band below the video ----
    ImGui::Separator();
    ImGui::Checkbox("Append script timeline below video", &st.overlayTimeline);
    if (st.overlayTimeline) {
        if (ImGui::SliderFloat("Timeline window", &st.timelineVisibleTime, 1.f, 60.f, "%.1f s"))
            st.timelineVisibleTime = Util::Clamp(st.timelineVisibleTime, 1.f, 300.f);
        if (ImGui::SliderFloat("Strip height", &st.timelineHeightFrac, 0.05f, 0.6f, "%.2f"))
            st.timelineHeightFrac = Util::Clamp(st.timelineHeightFrac, 0.05f, 0.6f);
        ImGui::Checkbox("Height lines", &st.timelineShowHeightLines);
        ImGui::SameLine();
        ImGui::Checkbox("Axis names", &st.timelineShowLabels);
        ImGui::Checkbox("Include all enabled scripts (multi-axis)", &st.timelineAllScripts);

        if (app->LoadedFunscripts().empty()) {
            ImGui::TextDisabled("No script loaded \xE2\x80\x94 nothing to draw.");
        } else if (vw > 0 && vh > 0) {
            const float previewW = Util::Min(ImGui::GetContentRegionAvail().x, 480.f);
            const float aspect = st.padTo169 ? (9.f / 16.f) : ((float)vh / (float)vw);
            const float stripH = previewW * aspect * st.timelineHeightFrac;
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            OFS_TimelineRaster::DrawInto(ImGui::GetWindowDrawList(), origin, ImVec2(previewW, stripH),
                              app->LoadedFunscripts(), (int)app->LoadedProject->ActiveIdx(),
                              (float)app->player->CurrentTime(), st.timelineVisibleTime,
                              st.timelineShowHeightLines, st.timelineShowLabels, st.timelineAllScripts);
            ImGui::Dummy(ImVec2(previewW, stripH));
        }
    }

    ImGui::Separator();
    const char* videoPath = app->player->VideoPath();
    const bool busy = exporting.load() || renderingFrames;
    const bool canExport = !busy && haveRange && videoPath && *videoPath;

    ImGui::BeginDisabled(!canExport);
    if (ImGui::Button("Export\xE2\x80\xA6")) {
        // Build the segment list: one slice for a normal export, or N evenly-spread
        // slices (each centered in its 1/N band of the timeline) for a compilation.
        std::vector<Segment> segments;
        if (st.compilation) {
            const float T = (float)duration;
            const int n = Util::Clamp(st.compSlices, 1, 100);
            const float D = Util::Clamp(st.compSliceDur, 0.1f, Util::Max(0.1f, T));
            const float band = T / (float)n;
            for (int i = 0; i < n; ++i) {
                float s = (float)i * band + (band - D) * 0.5f;
                s = Util::Clamp(s, 0.f, Util::Max(0.f, T - D));
                segments.push_back({ s, D });
            }
        } else {
            segments.push_back({ rStart, rEnd - rStart });
        }
        const char* ext = st.format == 0 ? ".webp" : ".mp4";
        const char* filter = st.format == 0 ? "*.webp" : "*.mp4";
        std::string defaultName = std::string(st.compilation ? "compilation" : "preview") + ext;
        Util::SaveFileDialog("Export animated preview", defaultName,
            [this, segments](Util::FileDialogResult& result) {
                if (!result.files.empty()) startExport(result.files[0], segments);
            },
            { filter });
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (renderingFrames) {
        ImGui::TextDisabled("Rendering frames %d/%d\xE2\x80\xA6", jobFrame, jobFrameCount);
    } else if (exporting.load()) {
        ImGui::TextDisabled("Encoding\xE2\x80\xA6");
    } else if (hasRun) {
        if (lastResult.load() == 0)
            ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f), "Done");
        else
            ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "ffmpeg failed (%d)", lastResult.load());
    }

    if (hasRun && !exporting.load() && !renderingFrames && lastResult.load() != 0 && !lastErrorText.empty()) {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.6f, 1.f), "%s", lastErrorText.c_str());
        ImGui::PopTextWrapPos();
    }

    if (st.format == 1)
        ImGui::TextDisabled("AV1 uses libsvtav1.");

    ImGui::End();
}
