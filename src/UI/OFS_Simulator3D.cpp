#include "OFS_Simulator3D.h"
#include "state/Simulator3DState.h"

#include "Funscript.h"
#include "OFS_Util.h"
#include "OFS_Localization.h"
#include "OFS_GL.h"
#include "OFS_Shader.h"
#include "OFS_ImGui.h"
#include "OFS_DynamicFontAtlas.h"

#include "imgui.h"
#include "stb_sprintf.h"

#include "glm/glm.hpp"
#include "glm/gtc/matrix_transform.hpp"
#include "glm/gtc/type_ptr.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>
#include <cstdint>

static constexpr float PI = 3.14159265358979323846f;

// Device presets, indexed by Simulator3DState::device.
enum Sim3DDevice : int32_t { Dev_All = 0,
    Dev_SR6 = 1,
    Dev_OSR2 = 2 };

// Whether a given axis is driven on the selected device. Per the TCode spec,
// OSR2+ supports L0/R0/R1/R2 only (no surge/sway); SR6 supports the six
// positional axes; the auxiliary channels (suck/vib/pump/raw) are separate
// hardware, so only the generic "All axes" preset drives them.
static bool AxisEnabled(int32_t device, const std::string& axis) noexcept
{
    const bool aux = (axis == "suck" || axis == "vib" || axis == "pump" || axis == "raw");
    if (device == Dev_OSR2 && (axis == "surge" || axis == "sway")) return false;
    if ((device == Dev_OSR2 || device == Dev_SR6) && aux) return false;
    return true;
}

// Resolve the axis a loaded script represents from its title suffix
// (e.g. "clip.surge" -> "surge"). Only suffixes matching a known axis name
// count; anything else is treated as the main stroke axis.
static std::string AxisSuffix(const std::string& title) noexcept
{
    auto dot = title.rfind('.');
    if (dot == std::string::npos) return { };
    std::string suffix = title.substr(dot + 1);
    for (auto* name : Funscript::AxisNames) {
        if (suffix == name) return suffix;
    }
    return { };
}

// --- Procedural mesh generation (interleaved position + normal, GL_TRIANGLES) ---
// A future external-model loader would fill the same interleaved buffer instead.
static void appendVertex(std::vector<float>& m, const glm::vec3& p, const glm::vec3& n) noexcept
{
    m.push_back(p.x);
    m.push_back(p.y);
    m.push_back(p.z);
    m.push_back(n.x);
    m.push_back(n.y);
    m.push_back(n.z);
}

static void appendQuad(std::vector<float>& m, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& d, const glm::vec3& n) noexcept
{
    appendVertex(m, a, n);
    appendVertex(m, b, n);
    appendVertex(m, c, n);
    appendVertex(m, a, n);
    appendVertex(m, c, n);
    appendVertex(m, d, n);
}

static void appendCylinder(std::vector<float>& m, float y0, float y1, float r, int seg) noexcept
{
    for (int i = 0; i < seg; ++i) {
        const float a0 = (float)i / seg * 2.f * PI;
        const float a1 = (float)(i + 1) / seg * 2.f * PI;
        const glm::vec3 n0(std::cos(a0), 0.f, std::sin(a0));
        const glm::vec3 n1(std::cos(a1), 0.f, std::sin(a1));
        const glm::vec3 p00 = glm::vec3(r * n0.x, y0, r * n0.z);
        const glm::vec3 p01 = glm::vec3(r * n0.x, y1, r * n0.z);
        const glm::vec3 p10 = glm::vec3(r * n1.x, y0, r * n1.z);
        const glm::vec3 p11 = glm::vec3(r * n1.x, y1, r * n1.z);
        // side (smooth radial normals)
        appendVertex(m, p00, n0);
        appendVertex(m, p10, n1);
        appendVertex(m, p11, n1);
        appendVertex(m, p00, n0);
        appendVertex(m, p11, n1);
        appendVertex(m, p01, n0);
        // caps
        appendVertex(m, glm::vec3(0, y1, 0), glm::vec3(0, 1, 0));
        appendVertex(m, p01, glm::vec3(0, 1, 0));
        appendVertex(m, p11, glm::vec3(0, 1, 0));
        appendVertex(m, glm::vec3(0, y0, 0), glm::vec3(0, -1, 0));
        appendVertex(m, p10, glm::vec3(0, -1, 0));
        appendVertex(m, p00, glm::vec3(0, -1, 0));
    }
}

static void appendBox(std::vector<float>& m, const glm::vec3& c, const glm::vec3& h) noexcept
{
    const glm::vec3 v[8] = {
        c + glm::vec3(-h.x, -h.y, -h.z),
        c + glm::vec3(h.x, -h.y, -h.z),
        c + glm::vec3(h.x, h.y, -h.z),
        c + glm::vec3(-h.x, h.y, -h.z),
        c + glm::vec3(-h.x, -h.y, h.z),
        c + glm::vec3(h.x, -h.y, h.z),
        c + glm::vec3(h.x, h.y, h.z),
        c + glm::vec3(-h.x, h.y, h.z),
    };
    appendQuad(m, v[4], v[5], v[6], v[7], glm::vec3(0, 0, 1));
    appendQuad(m, v[1], v[0], v[3], v[2], glm::vec3(0, 0, -1));
    appendQuad(m, v[5], v[1], v[2], v[6], glm::vec3(1, 0, 0));
    appendQuad(m, v[0], v[4], v[7], v[3], glm::vec3(-1, 0, 0));
    appendQuad(m, v[3], v[2], v[6], v[7], glm::vec3(0, 1, 0));
    appendQuad(m, v[0], v[1], v[5], v[4], glm::vec3(0, -1, 0));
}

// Capsule aligned to the Z axis (points forward), cylinder body + hemisphere caps.
static void appendCapsuleZ(std::vector<float>& m, const glm::vec3& c, float r, float hh, int seg, int rings) noexcept
{
    for (int i = 0; i < seg; ++i) { // cylinder body (radial normals in XY)
        const float a0 = (float)i / seg * 2.f * PI;
        const float a1 = (float)(i + 1) / seg * 2.f * PI;
        const glm::vec3 n0(std::cos(a0), std::sin(a0), 0.f);
        const glm::vec3 n1(std::cos(a1), std::sin(a1), 0.f);
        const glm::vec3 f0 = c + glm::vec3(0, 0, hh) + r * n0, k0 = c + glm::vec3(0, 0, -hh) + r * n0;
        const glm::vec3 f1 = c + glm::vec3(0, 0, hh) + r * n1, k1 = c + glm::vec3(0, 0, -hh) + r * n1;
        appendVertex(m, k0, n0);
        appendVertex(m, k1, n1);
        appendVertex(m, f1, n1);
        appendVertex(m, k0, n0);
        appendVertex(m, f1, n1);
        appendVertex(m, f0, n0);
    }
    auto hemi = [&](float sz) { // sz = +1 front, -1 back
        const glm::vec3 capC = c + glm::vec3(0, 0, sz * hh);
        auto V = [sz](float phi, float a) {
            return glm::vec3(std::cos(phi) * std::cos(a), std::cos(phi) * std::sin(a), sz * std::sin(phi));
        };
        for (int j = 0; j < rings; ++j) {
            const float p0 = (float)j / rings * (PI * 0.5f);
            const float p1 = (float)(j + 1) / rings * (PI * 0.5f);
            for (int i = 0; i < seg; ++i) {
                const float a0 = (float)i / seg * 2.f * PI;
                const float a1 = (float)(i + 1) / seg * 2.f * PI;
                const glm::vec3 n00 = V(p0, a0), n01 = V(p0, a1), n11 = V(p1, a1), n10 = V(p1, a0);
                appendVertex(m, capC + r * n00, n00);
                appendVertex(m, capC + r * n01, n01);
                appendVertex(m, capC + r * n11, n11);
                appendVertex(m, capC + r * n00, n00);
                appendVertex(m, capC + r * n11, n11);
                appendVertex(m, capC + r * n10, n10);
            }
        }
    };
    hemi(1.f);
    hemi(-1.f);
}

// Capsule along Y. `scale` stretches the shape per-axis (e.g. wide X + thin Z
// for a flat tongue); positions are scaled and normals inverse-scaled to stay correct.
static void appendCapsuleY(std::vector<float>& m, const glm::vec3& c, float r, float hh, int seg, int rings,
                           const glm::vec3& scale = glm::vec3(1.f)) noexcept
{
    // off = offset from center c
    auto push = [&](const glm::vec3& off, const glm::vec3& n) {
        appendVertex(m, c + scale * off, glm::normalize(n / scale));
    };
    for (int i = 0; i < seg; ++i) { // cylinder body
        const float a0 = (float)i / seg * 2.f * PI;
        const float a1 = (float)(i + 1) / seg * 2.f * PI;
        const glm::vec3 n0(std::cos(a0), 0.f, std::sin(a0));
        const glm::vec3 n1(std::cos(a1), 0.f, std::sin(a1));
        const glm::vec3 t(0.f, hh, 0.f), b(0.f, -hh, 0.f);
        push(b + r * n0, n0); push(b + r * n1, n1); push(t + r * n1, n1);
        push(b + r * n0, n0); push(t + r * n1, n1); push(t + r * n0, n0);
    }
    auto hemi = [&](float sy) { // sy = +1 top, -1 bottom
        const glm::vec3 capO(0.f, sy * hh, 0.f);
        auto V = [sy](float phi, float a) {
            return glm::vec3(std::cos(phi) * std::cos(a), sy * std::sin(phi), std::cos(phi) * std::sin(a));
        };
        for (int j = 0; j < rings; ++j) {
            const float p0 = (float)j / rings * (PI * 0.5f);
            const float p1 = (float)(j + 1) / rings * (PI * 0.5f);
            for (int i = 0; i < seg; ++i) {
                const float a0 = (float)i / seg * 2.f * PI;
                const float a1 = (float)(i + 1) / seg * 2.f * PI;
                const glm::vec3 n00 = V(p0, a0), n01 = V(p0, a1), n11 = V(p1, a1), n10 = V(p1, a0);
                push(capO + r * n00, n00); push(capO + r * n01, n01); push(capO + r * n11, n11);
                push(capO + r * n00, n00); push(capO + r * n11, n11); push(capO + r * n10, n10);
            }
        }
    };
    hemi(1.f);
    hemi(-1.f);
}

// Builds the device model matrix from a sampled pose (rotation + aux-axis
// deform + vib jitter). Shared by the interactive canvas and the export render.
static glm::mat4 poseModel(const Pose3D& pose, float jitterTime) noexcept
{
    // Matches the reference OFS_Simulator3D, which does:
    //   GlobalRotate(Right, pitch); GlobalRotate(Forward, roll); RotateObjectLocal(Up, twist)
    // Godot's GlobalRotate pre-multiplies and RotateObjectLocal post-multiplies, so the
    // composition is Rz(roll) * Rx(pitch) * Ry(twist): roll outermost, twist local/innermost
    // (twist spins the receiver about its own shaft rather than about world up).
    glm::mat4 rot = glm::rotate(glm::mat4(1.f), pose.rz, glm::vec3(0.f, 0.f, 1.f))
        * glm::rotate(glm::mat4(1.f), pose.rx, glm::vec3(1.f, 0.f, 0.f))
        * glm::rotate(glm::mat4(1.f), pose.ry, glm::vec3(0.f, 1.f, 0.f));
    glm::vec3 auxScale(1.f);
    if (pose.hasSuck) { const float s = 1.f - pose.suck * 0.35f; auxScale.x *= s; auxScale.z *= s; }
    if (pose.hasPump) { auxScale.y *= 1.f + (pose.pump * 2.f - 1.f) * 0.2f; }
    glm::vec3 vibJitter(0.f);
    if (pose.hasVib) {
        const float amp = pose.vib * 0.04f;
        vibJitter.x = std::sin(jitterTime * 47.f) * amp;
        vibJitter.z = std::sin(jitterTime * 39.f) * amp;
    }
    return glm::translate(glm::mat4(1.f), glm::vec3(pose.tx, pose.ty, pose.tz) + vibJitter)
        * rot * glm::scale(glm::mat4(1.f), auxScale);
}

Simulator3D::Simulator3D() noexcept = default;
Simulator3D::~Simulator3D() noexcept = default;

void Simulator3D::Init() noexcept
{
    stateHandle = OFS_AppState<Simulator3DState>::Register(Simulator3DState::StateName);
}

void Simulator3D::buildMesh() noexcept
{
    std::vector<float> mesh;
    // Match the industry-standard OFS_Simulator3D primitives: a stroker cylinder
    // plus two opposing twist-marker cubes (red left / purple right) so twist reads.
    const int v0 = 0;
    appendCylinder(mesh, -0.875f, 0.875f, 0.5f, 32);               // stroker (r0.5, height 1.75)
    const int v1 = (int)(mesh.size() / 6);
    appendBox(mesh, glm::vec3(-0.5f, 0.f, 0.f), glm::vec3(0.125f)); // twist marker L
    const int v2 = (int)(mesh.size() / 6);
    appendBox(mesh, glm::vec3(0.5f, 0.f, 0.f), glm::vec3(0.125f));  // twist marker R
    const int v3 = (int)(mesh.size() / 6);
    appendCapsuleY(mesh, glm::vec3(0.f, -0.90f, -0.4f), 0.18f, 0.22f, 20, 6,
                   glm::vec3(1.5f, 1.0f, 0.4f)); // tongue (back-bottom-center, wide & flat)
    const int v4 = (int)(mesh.size() / 6);
    appendBox(mesh, glm::vec3(0.f, 0.f, 0.5f), glm::vec3(0.11f));   // center indicator (front)
    const int v5 = (int)(mesh.size() / 6);
    partOffset[0] = v0; partCount[0] = v1 - v0; // shaft (stroker)
    partOffset[1] = v1; partCount[1] = v2 - v1; // twist L
    partOffset[2] = v2; partCount[2] = v3 - v2; // twist R
    partOffset[3] = v3; partCount[3] = v4 - v3; // tongue
    partOffset[4] = v4; partCount[4] = v5 - v4; // center
    meshVertexCount = v5;

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, mesh.size() * sizeof(float), mesh.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    // Thin unit vertical box (y 0..1); oriented/scaled per-frame for the stroke line.
    std::vector<float> lineMesh;
    appendBox(lineMesh, glm::vec3(0.f, 0.5f, 0.f), glm::vec3(0.012f, 0.5f, 0.012f));
    lineVertCount = (int)(lineMesh.size() / 6);
    glGenVertexArrays(1, &lineVao);
    glBindVertexArray(lineVao);
    glGenBuffers(1, &lineVbo);
    glBindBuffer(GL_ARRAY_BUFFER, lineVbo);
    glBufferData(GL_ARRAY_BUFFER, lineMesh.size() * sizeof(float), lineMesh.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
}

// (Re)creates an FBO + color texture + depth renderbuffer at w x h if the size changed.
static void ensureFboSet(uint32_t& fbo, uint32_t& colorTex, uint32_t& depthRbo,
                         int& curW, int& curH, int w, int h) noexcept
{
    if (fbo && w == curW && h == curH) return;
    if (fbo) {
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &colorTex);
        glDeleteRenderbuffers(1, &depthRbo);
    }

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    glGenTextures(1, &colorTex);
    glBindTexture(GL_TEXTURE_2D, colorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, OFS_InternalTexFormat, w, h, 0, OFS_TexFormat, GL_UNSIGNED_BYTE, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);

    glGenRenderbuffers(1, &depthRbo);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRbo);

    curW = w;
    curH = h;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Simulator3D::ensureGL(int width, int height) noexcept
{
    if (!shader) shader = std::make_unique<LightingShader>();
    if (!vao) buildMesh();
    ensureFboSet(fbo, colorTex, depthRbo, fbWidth, fbHeight, width, height);
}

void Simulator3D::ensureExportGL(int width, int height) noexcept
{
    if (!shader) shader = std::make_unique<LightingShader>();
    if (!vao) buildMesh();
    ensureFboSet(exFbo, exColorTex, exDepthRbo, exWidth, exHeight, width, height);
    ensureFboSet(ssFbo, ssColorTex, ssDepthRbo, ssWidth, ssHeight, width * 2, height * 2); // 2x supersample
}

void Simulator3D::renderMeshToTexture(uint32_t targetFbo, int w, int h, const float* model, const float* view,
                                      const float* proj, const float* camPos, bool transparent) noexcept
{
    // Save global GL state we touch so we never corrupt ImGui's own rendering.
    GLint prevFbo = 0, prevViewport[4] = {0, 0, 0, 0};
    GLboolean prevDepth = glIsEnabled(GL_DEPTH_TEST);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    glBindFramebuffer(GL_FRAMEBUFFER, targetFbo);
    glViewport(0, 0, w, h);
    // Transparent clear when compositing (overlay / export) so only the model shows.
    if (transparent)
        glClearColor(0.f, 0.f, 0.f, 0.f);
    else
        glClearColor(0.06f, 0.06f, 0.07f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    shader->Use();
    shader->ModelMtx(model);
    shader->ViewMtx(view);
    shader->ProjectionMtx(proj);
    shader->ViewPos(camPos);
    shader->LightPos(glm::value_ptr(glm::vec3(2.5f, 4.f, 3.f)));
    // lightColor has no public setter; set it directly so lighting is never black.
    const glm::vec3 lightColor(1.f, 1.f, 1.f);
    glUniform3fv(glGetUniformLocation(shader->Handle(), "lightColor"), 1, glm::value_ptr(lightColor));

    // Draw each part with its own color.
    auto& st = Simulator3DState::State(stateHandle);
    // part order: shaft, twist L (marker), twist R (base), tongue, center
    const ImColor* partColors[PartCount] = { &st.shaftColor, &st.markerColor, &st.baseColor,
                                             &st.tongueColor, &st.centerColor };
    glBindVertexArray(vao);
    for (int i = 0; i < PartCount; ++i) {
        if (partCount[i] <= 0) continue;
        if (i == 3 && !st.showTongue) continue; // tongue is optional
        if (i == 4 && !st.showCenter) continue; // center indicator is optional
        shader->ObjectColor(&partColors[i]->Value.x);
        glDrawArrays(GL_TRIANGLES, partOffset[i], partCount[i]);
    }
    glBindVertexArray(0);

    // Stroke line: from a fixed ground anchor to the (moving/rotating) cylinder bottom,
    // so it tilts and stretches with the device rather than staying vertical.
    if (st.showStrokeLine && lineVertCount > 0) {
        const glm::mat4 dm = glm::make_mat4(model);
        const glm::vec3 top = glm::vec3(dm * glm::vec4(0.f, -0.875f, 0.f, 1.f)); // cylinder bottom
        const glm::vec3 base(0.f, -2.f, 0.f);                                    // fixed ground anchor
        const glm::vec3 delta = top - base;
        const float len = glm::length(delta);
        if (len > 1e-4f) {
            // Orient the unit +Y box along the anchor->cylinder direction.
            const glm::vec3 d = delta / len;
            const glm::vec3 ref = (std::abs(d.y) > 0.99f) ? glm::vec3(1.f, 0.f, 0.f) : glm::vec3(0.f, 1.f, 0.f);
            const glm::vec3 right = glm::normalize(glm::cross(ref, d));
            const glm::vec3 fwd = glm::cross(d, right);
            glm::mat4 R(1.f);
            R[0] = glm::vec4(right, 0.f);
            R[1] = glm::vec4(d, 0.f);
            R[2] = glm::vec4(fwd, 0.f);
            const glm::mat4 lineModel = glm::translate(glm::mat4(1.f), base) * R
                * glm::scale(glm::mat4(1.f), glm::vec3(1.f, len, 1.f));
            shader->ModelMtx(glm::value_ptr(lineModel));
            const glm::vec4 lineCol(0.90f, 0.90f, 0.95f, 1.f);
            shader->ObjectColor(glm::value_ptr(lineCol));
            glBindVertexArray(lineVao);
            glDrawArrays(GL_TRIANGLES, 0, lineVertCount);
            glBindVertexArray(0);
        }
    }

    // Restore state.
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    if (!prevDepth) glDisable(GL_DEPTH_TEST);
}

Pose3D Simulator3D::samplePose(const std::vector<std::shared_ptr<Funscript>>& scripts, float time) noexcept
{
    auto& st = Simulator3DState::State(stateHandle);
    Pose3D pose;
    for (auto& script : scripts) {
        if (!script) continue;
        const std::string axis = AxisSuffix(script->Title());
        if (!AxisEnabled(st.device, axis)) continue; // device doesn't drive this axis
        // Center (50) when the channel has no actions, matching how a device rests.
        const float v = script->Actions().empty() ? 0.5f : (script->GetPositionAtTime(time) / 100.f); // 0..1
        const float bipolar = v * 2.f - 1.f;
        auto dir = [bipolar](bool invert) { return invert ? -bipolar : bipolar; };

        if (axis.empty()) {
            pose.ty = dir(st.invertStroke) * st.translateRange; pose.hasStroke = true;
            pose.stroke01 = v; // report the funscript position, not the display transform
        } else if (axis == "surge") {
            // Surge drives Z (toward/away from the default camera), negated like
            // sway/roll/pitch so it moves with the same handedness as the other axes.
            pose.tz = -dir(st.invertSurge) * st.translateRange * 0.5f; pose.hasSurge = true;
        } else if (axis == "sway") {
            pose.tx = -dir(st.invertSway) * st.translateRange * 0.5f; pose.hasSway = true;
        } else if (axis == "twist") {
            pose.ry = glm::radians(dir(st.invertTwist) * st.twistRange); pose.hasTwist = true;
        } else if (axis == "roll") {
            pose.rz = -glm::radians(dir(st.invertRoll) * st.rollRange); pose.hasRoll = true;
        } else if (axis == "pitch") {
            pose.rx = -glm::radians(dir(st.invertPitch) * st.pitchRange); pose.hasPitch = true;
        } else if (axis == "suck") {
            pose.suck = v; pose.hasSuck = true;
        } else if (axis == "vib") {
            pose.vib = v; pose.hasVib = true;
        } else if (axis == "pump") {
            pose.pump = v; pose.hasPump = true;
        }
    }
    return pose;
}

uint32_t Simulator3D::RenderPoseTexture(const std::vector<std::shared_ptr<Funscript>>& scripts, float time, int w, int h) noexcept
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    ensureExportGL(w, h);

    const Pose3D pose = samplePose(scripts, time);
    const glm::mat4 model = poseModel(pose, time); // frame time -> deterministic jitter
    auto& st = Simulator3DState::State(stateHandle);
    const glm::vec3 camPos = st.camDist * glm::vec3(0.f, 0.f, 1.f); // front-locked (yaw=pitch=0)
    const glm::mat4 view = glm::lookAt(camPos, glm::vec3(0.f), glm::vec3(0.f, 1.f, 0.f));
    const glm::mat4 proj = glm::perspective(glm::radians(45.f), (float)w / (float)h, 0.1f, 100.f);

    // Render at 2x into the supersampled FBO, then linearly downscale into exFbo (antialiasing).
    renderMeshToTexture(ssFbo, ssWidth, ssHeight, glm::value_ptr(model), glm::value_ptr(view),
                        glm::value_ptr(proj), glm::value_ptr(camPos), /*transparent=*/true);
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, ssFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, exFbo);
    glBlitFramebuffer(0, 0, ssWidth, ssHeight, 0, 0, exWidth, exHeight, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    return exColorTex;
}

bool Simulator3D::RenderPoseRGBA(const std::vector<std::shared_ptr<Funscript>>& scripts, float time, int w, int h,
                                 std::vector<uint8_t>& outRGBA) noexcept
{
    if (w < 1 || h < 1) return false;
    RenderPoseTexture(scripts, time, w, h);

    outRGBA.resize((size_t)w * (size_t)h * 4u);
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, exFbo);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, outRGBA.data());
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);

    // glReadPixels is bottom-up; flip to top-down for image writing.
    const size_t stride = (size_t)w * 4u;
    for (int y = 0; y < h / 2; ++y) {
        uint8_t* a = outRGBA.data() + (size_t)y * stride;
        uint8_t* b = outRGBA.data() + (size_t)(h - 1 - y) * stride;
        std::swap_ranges(a, a + stride, b);
    }
    return true;
}

void Simulator3D::ShowWindow(bool* open, const std::vector<std::shared_ptr<Funscript>>& scripts, float currentTime) noexcept
{
    if (!*open) return;
    OFS_PROFILE(__FUNCTION__);

    auto& st = Simulator3DState::State(stateHandle);

    ImGui::Begin(TR_ID(WindowId, Tr::SIMULATOR_3D), open, ImGuiWindowFlags_None);

    // ---- Sample the loaded axes into a single rigid-body pose ----
    Pose3D pose = samplePose(scripts, currentTime);

    // ---- Controls / readout (kept above the canvas so it fills the rest) ----
    if (ImGui::CollapsingHeader("Options")) {
        ImGui::TextDisabled("Drag to orbit \xE2\x80\xA2 scroll to zoom");
        ImGui::Combo("Device", &st.device, "All axes\0SR6\0OSR2+\0\0");
        ImGui::Checkbox("Lit 3D (GPU)", &st.litMode);
        ImGui::SameLine();
        ImGui::Checkbox("Overlay on video", &st.overlayMode);
        ImGui::BeginDisabled(st.litMode);
        ImGui::Checkbox("Ground grid", &st.showGrid);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox("Axis gizmo", &st.showGizmo);
        ImGui::SameLine();
        ImGui::Checkbox("Stroke line", &st.showStrokeLine);
        ImGui::SameLine();
        ImGui::Checkbox("Height value", &st.showHeightText);
        OFS::Tooltip("Show the stroke position (0-100) under the model.");
        if (st.showHeightText) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.f);
            if (ImGui::SliderFloat("Size##heightText", &st.heightTextScale, 0.5f, 8.f, "%.1fx"))
                st.heightTextScale = Util::Clamp(st.heightTextScale, 0.5f, 8.f);
            OFS::Tooltip("Readout size, relative to the UI font.");
        }

        if (ImGui::Button("Recenter view")) {
            const Simulator3DState def{}; // reset camera to struct defaults
            st.camYaw = def.camYaw;
            st.camPitch = def.camPitch;
            st.camDist = def.camDist;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset ranges")) {
            const Simulator3DState def{}; // reference-matching motion ranges
            st.translateRange = def.translateRange;
            st.twistRange = def.twistRange;
            st.rollRange = def.rollRange;
            st.pitchRange = def.pitchRange;
        }
        OFS::Tooltip("Restore stroke/twist/roll/pitch ranges to the reference simulator's values.");

        // Range controls: slider for feel + editable box for exact values.
        auto rangeCtrl = [](const char* label, float* v, float mn, float mx) {
            ImGui::PushID(label);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
            ImGui::SliderFloat("##s", v, mn, mx);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70.f);
            if (ImGui::InputFloat("##i", v, 0.f, 0.f, "%.2f"))
                *v = Util::Clamp(*v, mn, mx);
            ImGui::SameLine();
            ImGui::TextUnformatted(label);
            ImGui::PopID();
        };
        rangeCtrl("Translate range", &st.translateRange, 0.2f, 3.f);
        rangeCtrl("Twist range", &st.twistRange, 0.f, 180.f);
        rangeCtrl("Roll range", &st.rollRange, 0.f, 90.f);
        rangeCtrl("Pitch range", &st.pitchRange, 0.f, 90.f);

        ImGui::Separator();
        ImGui::TextDisabled("Part colors");
        ImGui::ColorEdit3("Shaft", &st.shaftColor.Value.x, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::ColorEdit3("Twist L", &st.markerColor.Value.x, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::ColorEdit3("Twist R", &st.baseColor.Value.x, ImGuiColorEditFlags_NoInputs);

        ImGui::Checkbox("Tongue", &st.showTongue);
        ImGui::SameLine();
        ImGui::ColorEdit3("##tongueCol", &st.tongueColor.Value.x, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::Checkbox("Center", &st.showCenter);
        ImGui::SameLine();
        ImGui::ColorEdit3("##centerCol", &st.centerColor.Value.x, ImGuiColorEditFlags_NoInputs);

        ImGui::Separator();
        ImGui::TextDisabled("Invert axis");
        auto invert = [&](const char* label, const char* axis, bool* flag) {
            ImGui::BeginDisabled(!AxisEnabled(st.device, axis));
            ImGui::Checkbox(label, flag);
            ImGui::EndDisabled();
        };
        invert("stroke L0##inv", "", &st.invertStroke);   ImGui::SameLine();
        invert("surge L1##inv", "surge", &st.invertSurge); ImGui::SameLine();
        invert("sway L2##inv", "sway", &st.invertSway);
        invert("twist R0##inv", "twist", &st.invertTwist); ImGui::SameLine();
        invert("roll R1##inv", "roll", &st.invertRoll);   ImGui::SameLine();
        invert("pitch R2##inv", "pitch", &st.invertPitch);

        ImGui::Separator();
        ImGui::TextDisabled("Active axes");
        auto axisLabel = [](const char* name, bool active) {
            ImGui::TextColored(active ? ImVec4(1, 1, 1, 1) : ImVec4(0.5f, 0.5f, 0.5f, 1), "%s", name);
        };
        axisLabel("stroke", pose.hasStroke); ImGui::SameLine();
        axisLabel("surge", pose.hasSurge);   ImGui::SameLine();
        axisLabel("sway", pose.hasSway);     ImGui::SameLine();
        axisLabel("twist", pose.hasTwist);   ImGui::SameLine();
        axisLabel("roll", pose.hasRoll);     ImGui::SameLine();
        axisLabel("pitch", pose.hasPitch);
        axisLabel("suck", pose.hasSuck);     ImGui::SameLine();
        axisLabel("vib", pose.hasVib);       ImGui::SameLine();
        axisLabel("pump", pose.hasPump);
    }

    if (!st.overlayMode) {
        renderCanvas(st, pose, scripts, false);
    } else {
        ImGui::TextDisabled("Shown as an overlay on the video.");
        ImGui::TextDisabled("Uncheck \"Overlay on video\" to dock it here.");
    }

    ImGui::End();

    // Floating, borderless overlay drawn over the video. No title bar: drag the
    // body to move, drag the bottom-right corner to resize. A thin frame + resize
    // corner fade in on hover for discoverability; otherwise only the model shows.
    if (st.overlayMode) {
        ImGui::SetNextWindowSize(ImVec2(320.f, 320.f), ImGuiCond_FirstUseEver);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
        if (ImGui::Begin("###SIMULATOR_3D_OVERLAY", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDocking
                | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
                | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoCollapse)) {
            renderCanvas(st, pose, scripts, true);

            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
                const ImVec2 p = ImGui::GetWindowPos();
                const ImVec2 s = ImGui::GetWindowSize();
                const ImVec2 br(p.x + s.x, p.y + s.y);
                auto* fdl = ImGui::GetForegroundDrawList();
                fdl->AddRect(p, br, IM_COL32(0xFF, 0xFF, 0xFF, 0x66), 2.f, 0, 1.5f);
                fdl->AddTriangleFilled(ImVec2(br.x - 14.f, br.y), br, ImVec2(br.x, br.y - 14.f),
                                       IM_COL32(0xFF, 0xFF, 0xFF, 0x99));
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }
}

void Simulator3D::renderCanvas(Simulator3DState& st, const Pose3D& pose,
                               const std::vector<std::shared_ptr<Funscript>>& scripts, bool overlay) noexcept
{
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.y < 80.f) size.y = 80.f;
    if (size.x < 80.f) size.x = 80.f;

    auto& io = ImGui::GetIO();
    auto applyZoom = [&]() {
        if (io.MouseWheel != 0.f) {
            st.camDist *= (1.f - io.MouseWheel * 0.1f);
            st.camDist = Util::Clamp(st.camDist, 1.5f, 25.f);
        }
    };
    if (!overlay) {
        ImGui::InvisibleButton("##canvas3d", size);
        if (ImGui::IsItemActive()) {
            st.camYaw -= io.MouseDelta.x * 0.01f;
            st.camPitch += io.MouseDelta.y * 0.01f;
            st.camPitch = Util::Clamp(st.camPitch, -1.45f, 1.45f);
        }
        if (ImGui::IsItemHovered()) applyZoom();
    } else {
        ImGui::Dummy(size); // reserve space without capturing drag, so the overlay window stays movable
        if (ImGui::IsWindowHovered()) applyZoom(); // zoom (but not orbit) while hovering the overlay
    }

    auto* dl = ImGui::GetWindowDrawList();

    // ---- Build matrices ----
    const float yaw = overlay ? 0.f : st.camYaw;     // overlay locks front-and-center
    const float pitch = overlay ? 0.f : st.camPitch;
    const float aspect = size.x / size.y;
    glm::vec3 camPos = st.camDist * glm::vec3(
        std::cos(pitch) * std::sin(yaw),
        std::sin(pitch),
        std::cos(pitch) * std::cos(yaw));

    glm::mat4 model = poseModel(pose, (float)ImGui::GetTime());
    glm::mat4 view = glm::lookAt(camPos, glm::vec3(0.f), glm::vec3(0.f, 1.f, 0.f));
    glm::mat4 proj = glm::perspective(glm::radians(45.f), aspect, 0.1f, 100.f);
    glm::mat4 mv = view * model;
    glm::mat4 mvp = proj * mv;

    auto projectVP = [&](const glm::mat4& m, const glm::vec3& p, ImVec2& out) -> bool {
        glm::vec4 clip = m * glm::vec4(p, 1.f);
        if (clip.w <= 0.0001f) return false;
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        out = ImVec2(origin.x + (ndc.x * 0.5f + 0.5f) * size.x,
                     origin.y + (1.f - (ndc.y * 0.5f + 0.5f)) * size.y);
        return true;
    };
    auto project = [&](const glm::vec3& p, ImVec2& out) { return projectVP(mvp, p, out); };

    bool anyDrawn = false;

    if (st.litMode) {
        // ---- GPU path: render the lit mesh to a texture, blit into the canvas ----
        ensureGL((int)size.x, (int)size.y);
        renderMeshToTexture(fbo, fbWidth, fbHeight, glm::value_ptr(model), glm::value_ptr(view),
                            glm::value_ptr(proj), glm::value_ptr(camPos), /*transparent=*/overlay);
        // FBO textures are bottom-up, so flip V.
        dl->AddImage((ImTextureID)(intptr_t)colorTex, origin, origin + size,
                     ImVec2(0, 1), ImVec2(1, 0));
        anyDrawn = meshVertexCount > 0;
    } else {
        // ---- Fast path: flat box + optional grid through the draw list ----
        if (!overlay) // transparent over the video in overlay mode
            dl->AddRectFilled(origin, origin + size, IM_COL32(0x10, 0x10, 0x10, 0xFF), 4.f);
        dl->PushClipRect(origin, origin + size, true);

        if (st.showGrid && !overlay) {
            const glm::mat4 gridVP = proj * view;
            const float floorY = -2.f;
            const int lines = 10;
            const float step = 0.5f;
            const float ext = lines * step * 0.5f;
            const ImU32 gcol = IM_COL32(0x44, 0x4A, 0x50, 0xFF);
            for (int i = 0; i <= lines; ++i) {
                const float t = -ext + i * step;
                ImVec2 a, b;
                if (projectVP(gridVP, glm::vec3(t, floorY, -ext), a) && projectVP(gridVP, glm::vec3(t, floorY, ext), b))
                    dl->AddLine(a, b, gcol, 1.f);
                if (projectVP(gridVP, glm::vec3(-ext, floorY, t), a) && projectVP(gridVP, glm::vec3(ext, floorY, t), b))
                    dl->AddLine(a, b, gcol, 1.f);
            }
        }

        const glm::vec3 he(0.6f, 0.9f, 0.6f);
        const std::array<glm::vec3, 8> corners = {{
            {-he.x, -he.y, -he.z}, { he.x, -he.y, -he.z}, { he.x,  he.y, -he.z}, {-he.x,  he.y, -he.z},
            {-he.x, -he.y,  he.z}, { he.x, -he.y,  he.z}, { he.x,  he.y,  he.z}, {-he.x,  he.y,  he.z},
        }};
        struct Face { int idx[4]; glm::vec3 normal; ImU32 color; };
        const ImU32 sideCol = st.shaftColor;  // sides = shaft
        const ImU32 topCol  = st.markerColor; // top   = marker
        const ImU32 botCol  = st.baseColor;   // bottom = base
        const std::array<Face, 6> faces = {{
            {{4, 5, 6, 7}, { 0,  0,  1}, sideCol}, {{1, 0, 3, 2}, { 0,  0, -1}, sideCol},
            {{5, 1, 2, 6}, { 1,  0,  0}, sideCol}, {{0, 4, 7, 3}, {-1,  0,  0}, sideCol},
            {{3, 7, 6, 2}, { 0,  1,  0}, topCol }, {{0, 1, 5, 4}, { 0, -1,  0}, botCol },
        }};
        const glm::mat3 normalMat = glm::mat3(model);
        const glm::vec3 lightDir = glm::normalize(glm::vec3(0.4f, 0.8f, 0.6f));

        std::array<int, 6> order = {0, 1, 2, 3, 4, 5};
        std::array<float, 6> depth{};
        for (int f = 0; f < 6; ++f) {
            float z = 0.f;
            for (int k = 0; k < 4; ++k)
                z += (mv * glm::vec4(corners[faces[f].idx[k]], 1.f)).z;
            depth[f] = z * 0.25f;
        }
        std::sort(order.begin(), order.end(), [&](int a, int b) { return depth[a] < depth[b]; });

        for (int f : order) {
            const Face& face = faces[f];
            ImVec2 pts[4];
            bool ok = true;
            for (int k = 0; k < 4 && ok; ++k)
                ok = project(corners[face.idx[k]], pts[k]);
            if (!ok) continue;
            glm::vec3 n = glm::normalize(normalMat * face.normal);
            float shade = 0.35f + 0.65f * std::max(0.f, glm::dot(n, lightDir));
            ImU32 c = face.color;
            auto scale = [&](int shift) { return (ImU32)(((c >> shift) & 0xFF) * shade); };
            ImU32 shaded = IM_COL32(scale(IM_COL32_R_SHIFT), scale(IM_COL32_G_SHIFT), scale(IM_COL32_B_SHIFT), 0xFF);
            dl->AddQuadFilled(pts[0], pts[1], pts[2], pts[3], shaded);
            dl->AddQuad(pts[0], pts[1], pts[2], pts[3], IM_COL32(0, 0, 0, 0x80), 1.5f);
            anyDrawn = true;
        }
        dl->PopClipRect();
    }

    // ---- Object axis gizmo (overlay in both modes; reads orientation) ----
    if (st.showGizmo) {
        dl->PushClipRect(origin, origin + size, true);
        auto axis = [&](const glm::vec3& localEnd, ImU32 col, const char* label) {
            ImVec2 a, b;
            if (project(glm::vec3(0.f), a) && project(localEnd, b)) {
                dl->AddLine(a, b, col, 2.f);
                dl->AddText(b, col, label);
            }
        };
        axis(glm::vec3(1.4f, 0.f, 0.f), IM_COL32(0xFF, 0x50, 0x50, 0xFF), "X");
        axis(glm::vec3(0.f, 1.4f, 0.f), IM_COL32(0x50, 0xFF, 0x50, 0xFF), "Y");
        axis(glm::vec3(0.f, 0.f, 1.4f), IM_COL32(0x60, 0x90, 0xFF, 0xFF), "Z");
        dl->PopClipRect();
    }

    if (!anyDrawn) {
        ImVec2 center(origin.x + size.x * 0.5f, origin.y + size.y * 0.5f);
        const char* msg = scripts.empty() ? "No script loaded" : "Load a multi-axis script";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f),
                    IM_COL32(0xAA, 0xAA, 0xAA, 0xFF), msg);
    }

    // ---- Stroke position readout, centered under the model ----
    if (st.showHeightText && pose.hasStroke) {
        char b[8];
        stbsp_snprintf(b, sizeof(b), "%d", (int)std::lround(pose.stroke01 * 100.f));
        const float fontPx = ImGui::GetFontSize() * Util::Clamp(st.heightTextScale, 0.5f, 8.f);
        // Below the UI size the native font is sharpest; above it, scale down from
        // the large digits face instead of upscaling the UI font (which goes soft).
        ImFont* font = ImGui::GetFont();
        if (OFS_DynFontAtlas::NumberFont && fontPx > ImGui::GetFontSize())
            font = OFS_DynFontAtlas::NumberFont;
        const ImVec2 ts = font->CalcTextSizeA(fontPx, FLT_MAX, 0.f, b);
        const ImVec2 tp(origin.x + (size.x - ts.x) * 0.5f, origin.y + size.y - ts.y - 4.f);
        const float sh = Util::Max(1.f, fontPx * 0.06f); // shadow offset scales with the text
        dl->AddText(font, fontPx, ImVec2(tp.x + sh, tp.y + sh), IM_COL32(0, 0, 0, 0xC0), b);
        dl->AddText(font, fontPx, tp, IM_COL32(0xFF, 0xFF, 0xFF, 0xFF), b);
    }
}
