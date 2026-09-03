#pragma once

#include <cstdint>
#include <memory>
#include <vector>

class Funscript;
class LightingShader;
struct Simulator3DState;

// Rigid-body pose sampled from the loaded axes for one frame.
struct Pose3D {
    float tx = 0.f, ty = 0.f, tz = 0.f;    // translation (sway, up, surge)
    float rx = 0.f, ry = 0.f, rz = 0.f;    // rotation radians (pitch X, twist Y, roll Z)
    float suck = 0.f, vib = 0.f, pump = 0.f; // 0..1 (pump centered at 0.5)
    float stroke01 = 0.5f;                 // raw stroke axis value 0..1 (pre invert/range)
    bool hasStroke = false, hasSurge = false, hasSway = false;
    bool hasTwist = false, hasRoll = false, hasPitch = false;
    bool hasSuck = false, hasVib = false, hasPump = false;
};

// A 3D visualizer for multi-axis funscripts.
// Samples the loaded axis scripts at the current playback time and drives a
// rigid-body pose from up-down (main stroke) + surge/sway/twist/roll/pitch,
// plus auxiliary suck/vib/pump deformation.
//
// Two render paths, selected by Simulator3DState::litMode:
//   * fast   - flat box drawn through the ImGui draw list (grid + gizmo)
//   * lit 3D - a procedural device mesh rendered to an FBO with the existing
//              LightingShader (real depth testing + Phong lighting), shown via
//              ImGui::Image. The mesh is generated in code; a future external
//              model loader can replace buildMesh() without touching the rest.
class Simulator3D {
private:
    uint32_t stateHandle = 0xFFFF'FFFFu;

    // GL render-to-texture resources (created lazily on first lit-mode use).
    std::unique_ptr<LightingShader> shader;
    uint32_t fbo = 0, colorTex = 0, depthRbo = 0;
    int fbWidth = 0, fbHeight = 0;
    // Separate FBO for offscreen preview/export renders so they don't clash with
    // the interactive window's fbo when both render in the same frame.
    uint32_t exFbo = 0, exColorTex = 0, exDepthRbo = 0;
    int exWidth = 0, exHeight = 0;
    // Supersampled FBO: the mesh is rendered here at 2x then downscaled into exFbo
    // for antialiasing (smooth edges compress far better, esp. for lossless export).
    uint32_t ssFbo = 0, ssColorTex = 0, ssDepthRbo = 0;
    int ssWidth = 0, ssHeight = 0;
    uint32_t vao = 0, vbo = 0;
    int meshVertexCount = 0;
    // Unit vertical box (y 0..1) drawn with a dynamic transform for the stroke-length line.
    uint32_t lineVao = 0, lineVbo = 0;
    int lineVertCount = 0;
    // Submesh vertex ranges for the colorable parts:
    // 0=shaft, 1=twistL, 2=twistR, 3=tongue, 4=center.
    static constexpr int PartCount = 5;
    int partOffset[PartCount] = {0};
    int partCount[PartCount] = {0};

    void buildMesh() noexcept;
    void ensureGL(int width, int height) noexcept;
    void ensureExportGL(int width, int height) noexcept;
    // Renders the mesh into targetFbo (sized w x h) with the given matrices.
    void renderMeshToTexture(uint32_t targetFbo, int w, int h, const float* model, const float* view,
                             const float* proj, const float* camPos, bool transparent) noexcept;
    Pose3D samplePose(const std::vector<std::shared_ptr<Funscript>>& scripts, float time) noexcept;
    // Renders the 3D scene into the current ImGui window. In overlay mode the
    // camera is locked front-and-center, orbit is disabled, and the background
    // is transparent so the video shows through.
    void renderCanvas(Simulator3DState& st, const Pose3D& pose,
                      const std::vector<std::shared_ptr<Funscript>>& scripts, bool overlay) noexcept;

public:
    static constexpr const char* WindowId = "###SIMULATOR_3D";

    Simulator3D() noexcept;
    ~Simulator3D() noexcept; // defined in .cpp (LightingShader is incomplete here)

    void Init() noexcept;
    void ShowWindow(bool* open, const std::vector<std::shared_ptr<class Funscript>>& scripts, float currentTime) noexcept;

    // Offscreen render of the sim pose at `time` with a front-locked camera and
    // transparent background, for the preview export. RenderPoseTexture returns a
    // GL texture id; RenderPoseRGBA reads it back into a top-down RGBA buffer.
    // Must be called on the main (GL) thread. Requires the lit render path.
    uint32_t RenderPoseTexture(const std::vector<std::shared_ptr<Funscript>>& scripts, float time, int w, int h) noexcept;
    bool RenderPoseRGBA(const std::vector<std::shared_ptr<Funscript>>& scripts, float time, int w, int h,
                        std::vector<uint8_t>& outRGBA) noexcept;
};
