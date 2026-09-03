#pragma once

#include <cstdint>
#include <atomic>
#include <string>
#include <vector>

// Exports a short animated preview (WebP or AV1) of a chapter or timestamp range
// via ffmpeg. Runs the encode on a background thread so the UI stays responsive.
// (On-video simulator compositing is a planned follow-up, not included here.)
class OFS_PreviewExporter
{
private:
    uint32_t stateHandle = 0xFFFF'FFFFu;
    int selectedChapter = 0;
    float startTime = 0.f;
    float endTime = 0.f;

    std::atomic<bool> exporting{false};
    std::atomic<int> lastResult{0};
    bool hasRun = false;
    std::string lastErrorText; // captured ffmpeg stderr on failure

    // HH:MM:SS.mmm text buffers for the timestamp range fields.
    std::string startStr, endStr;
    bool startEditing = false, endEditing = false;
    bool draggingResize = false; // true while a drag that started in the resize corner is active

    // One compositing overlay stream (rendered to a PNG sequence, fed to ffmpeg).
    enum class LayerKind { Sim3D, Sim2D, Timeline };
    struct ExportLayer {
        LayerKind kind;
        int w = 0, h = 0;
        std::string tempDir;    // its own frame directory
        std::string filePrefix; // e.g. "frame_" / "tl_"
        std::vector<uint8_t> pixels;
    };

    // A section of the source to export: [start, start+dur]. A normal export is one
    // segment; a compilation is several spread across the media, concatenated.
    struct Segment { float start; float dur; };

    // Incremental frame-render job. Each enabled layer's frame is rendered a few
    // per UI frame on the main thread (GL), then ffmpeg runs on a background thread.
    // Output frame k maps to a source time within one segment (see Update()).
    bool renderingFrames = false;
    int jobFrame = 0, jobFrameCount = 0;
    float jobFps = 15.f;
    std::vector<float> jobSegStarts;   // source start time of each segment
    int jobFramesPerSeg = 0;           // output frames per segment (equal durations)
    std::vector<ExportLayer> jobLayers;
    std::vector<std::string> jobArgs;

    void startExport(const std::string& outputPath, const std::vector<Segment>& segments) noexcept;
    void spawnFfmpeg(std::vector<std::string> args, std::vector<std::string> tempDirs) noexcept;

public:
    static constexpr const char* WindowId = "###PREVIEW_EXPORT";

    void Init() noexcept;
    void Update() noexcept; // advances the frame-render job; call every frame
    void ShowWindow(bool* open) noexcept;
    // Opens the exporter pre-set to a chapter's range (from the chapter context menu).
    void OpenForChapter(float start, float end) noexcept;
};
