#include "FunscriptHeatmap.h"
#include "OFS_Util.h"
#include "OFS_Profiling.h"

#include "OFS_ImGui.h"
#include "OFS_Shader.h"
#include "OFS_GL.h"

#include <chrono>
#include <memory>
#include <array>

ImGradient FunscriptHeatmap::LineColors;

static constexpr auto SpeedTextureResolution = 2048;

class HeatmapShader : public ShaderBase
{
private:
	int32_t ProjMtxLoc = 0;
    int32_t SpeedLoc = 0;

	static constexpr const char* vtx_shader = OFS_SHADER_VERSION R"(
		precision highp float;

		uniform mat4 ProjMtx;
		in vec2 Position;
		in vec2 UV;
		in vec4 Color;
		out vec2 Frag_UV;
		out vec4 Frag_Color;
		
		void main()	{
			Frag_UV = UV;
			Frag_Color = Color;
			gl_Position = ProjMtx * vec4(Position.xy, 0, 1);
		}
	)";

	static constexpr const char* frag_shader = OFS_SHADER_VERSION R"(
		precision highp float;
        uniform sampler2D speedTex;

		in vec2 Frag_UV;
		in vec4 Frag_Color;
		out vec4 Out_Color;

        const vec3 colors[] = vec3[]( // speed, name, #hex (og #hex @ alpha)
            vec3(0., 0., 0.) / 255.,       // 0    black   #000000 (og #00eeff @ 0.0)
            vec3(0., 128., 122.) / 255.,   // 50   cyan    #00807a (og #00fff3 @ 0.5)
        //  vec3(0., 238., 255.) / 255.,   // 0    cyan    #00eeff
        //  vec3(0., 255., 243.) / 255.,   // 50   cyan    #00fff3
            vec3(0., 255., 138.) / 255.,   // 100  lime    #00ff8a
            vec3(0., 247., 0.) / 255.,     // 150  lime    #00f700
            vec3(120., 224., 0.) / 255.,   // 200  lime    #78e000
            vec3(232., 189., 0.) / 255.,   // 250  gold    #e8bd00
            vec3(255., 140., 0.) / 255.,   // 300  orange  #ff8c00
            vec3(255., 64., 0.) / 255.,    // 350  orange  #ff4000
            vec3(255., 0., 0.) / 255.,     // 400  red     #ff0000
            vec3(255., 0., 30.) / 255.,    // 450  red     #ff001e
            vec3(255., 0., 171.) / 255.,   // 500  magenta #ff00ab
            vec3(255., 0., 196.) / 255.,   // 550  magenta #ff00c4
            vec3(150., 0., 197.) / 255.,   // 600  violet  #9600c5
            vec3(119., 0., 249.) / 255.,   // 650  purple  #7700f9
            vec3(82., 0., 255.) / 255.,    // 700  blue    #5200ff
            vec3(0., 0., 255.) / 255.,     // 750  blue    #0000ff
            vec3(0., 3., 254.) / 255.,     // 800  blue    #0003fe
            vec3(0., 90., 155.) / 255.,    // 850  blue    #005a9b
            vec3(0., 87., 88.) / 255.,     // 900  teal    #005758
            vec3(0., 88., 68.) / 255.,     // 950  teal    #005844
            vec3(4., 87., 45.) / 255.,     // 1000 green   #04572d
            vec3(50., 82., 16.) / 255.,    // 1050 green   #325210
            vec3(74., 76., 0.) / 255.,     // 1100 olive   #4a4c00
            vec3(92., 68., 0.) / 255.,     // 1150 olive   #5c4400
            vec3(105., 60., 0.) / 255.,    // 1200 brown   #693c00
            vec3(113., 52., 10.) / 255.,   // 1250 brown   #71340a
            vec3(116., 46., 39.) / 255.,   // 1300 maroon  #742e27
            vec3(115., 45., 62.) / 255.,   // 1350 maroon  #732d3e
            vec3(109., 46., 82.) / 255.,   // 1400 purple  #6d2e52
            vec3(98., 50., 100.) / 255.,   // 1450 purple  #623264
            vec3(84., 55., 114.) / 255.,   // 1500 indigo  #543772
            vec3(66., 62., 123.) / 255.,   // 1550 indigo  #423e7b
            vec3(42., 69., 125.) / 255.,   // 1600 navy    #2a457d
            vec3(0., 76., 120.) / 255.,    // 1650 navy    #004c78
            vec3(0., 82., 110.) / 255.,    // 1700 teal    #00526e
            vec3(0., 86., 93.) / 255.,     // 1750 teal    #00565d
            vec3(0., 88., 74.) / 255.,     // 1800 green   #00584a
            vec3(0., 87., 51.) / 255.,     // 1850 green   #005733
            vec3(41., 84., 25.) / 255.,    // 1900 green   #295419
            vec3(68., 78., 0.) / 255.,     // 1950 olive   #444e00
            vec3(87., 70., 0.) / 255.      // 2000 olive   #574600
        );

        vec3 RAMP(vec3 cols[41], float x) {
            x *= float(cols.length() - 1);
            int idx = int(x);
            idx = min(idx, cols.length() - 2);
            return mix(cols[idx], cols[idx + 1], smoothstep(0.0, 1.0, fract(x)));
        }

		void main()	{
            float speed = texture(speedTex, vec2(Frag_UV.x, 0.f)).r;
            vec3 color = RAMP(colors, speed);
            color = mix(vec3(0.f, 0.f, 0.f), color, Frag_UV.y);
            Out_Color = vec4(color, 1.f);
		}
	)";

	void initUniformLocations() noexcept;
public:
	HeatmapShader()
		: ShaderBase(vtx_shader, frag_shader)
	{
		initUniformLocations();
	}

	void ProjMtx(const float* mat4) noexcept;
    void SpeedTex(uint32_t unit) noexcept;
};

static std::unique_ptr<HeatmapShader> Shader;


void HeatmapShader::initUniformLocations() noexcept
{
	ProjMtxLoc = glGetUniformLocation(program, "ProjMtx");
    SpeedLoc = glGetUniformLocation(program, "speedTex");
}

void HeatmapShader::ProjMtx(const float* mat4) noexcept
{
	glUniformMatrix4fv(ProjMtxLoc, 1, GL_FALSE, mat4);
}

void HeatmapShader::SpeedTex(uint32_t unit) noexcept
{
    glUniform1i(SpeedLoc, unit);
}

void FunscriptHeatmap::Init() noexcept
{
    if(LineColors.getMarks().empty())
    {
        // Speed colors 0-2000 (step 50), blended with white for low speeds
        // speed, name, #hex (og #hex @ alpha)
        std::array<ImColor, 41> heatColor {
        //  IM_COL32(0xFF, 0xFF, 0xFF, 0xFF),  // 0    white   #ffffff (og #00eeff @ 0.0)
        //  IM_COL32(0x80, 0xFF, 0xF9, 0xFF),  // 50   cyan    #80fff9 (og #00fff3 @ 0.5)
            IM_COL32(0x00, 0xEE, 0xFF, 0xFF),  // 0    cyan    #00eeff
            IM_COL32(0x00, 0xFF, 0xF3, 0xFF),  // 50   cyan    #00fff3
            IM_COL32(0x00, 0xFF, 0x8A, 0xFF),  // 100  lime    #00ff8a
            IM_COL32(0x00, 0xF7, 0x00, 0xFF),  // 150  lime    #00f700
            IM_COL32(0x78, 0xE0, 0x00, 0xFF),  // 200  lime    #78e000
            IM_COL32(0xE8, 0xBD, 0x00, 0xFF),  // 250  gold    #e8bd00
            IM_COL32(0xFF, 0x8C, 0x00, 0xFF),  // 300  orange  #ff8c00
            IM_COL32(0xFF, 0x40, 0x00, 0xFF),  // 350  orange  #ff4000
            IM_COL32(0xFF, 0x00, 0x00, 0xFF),  // 400  red     #ff0000
            IM_COL32(0xFF, 0x00, 0x1E, 0xFF),  // 450  red     #ff001e
            IM_COL32(0xFF, 0x00, 0xAB, 0xFF),  // 500  magenta #ff00ab
            IM_COL32(0xFF, 0x00, 0xC4, 0xFF),  // 550  magenta #ff00c4
            IM_COL32(0x96, 0x00, 0xC5, 0xFF),  // 600  violet  #9600c5
            IM_COL32(0x77, 0x00, 0xF9, 0xFF),  // 650  purple  #7700f9
            IM_COL32(0x52, 0x00, 0xFF, 0xFF),  // 700  blue    #5200ff
            IM_COL32(0x00, 0x00, 0xFF, 0xFF),  // 750  blue    #0000ff
            IM_COL32(0x00, 0x03, 0xFE, 0xFF),  // 800  blue    #0003fe
            IM_COL32(0x00, 0x5A, 0x9B, 0xFF),  // 850  blue    #005a9b
            IM_COL32(0x00, 0x57, 0x58, 0xFF),  // 900  teal    #005758
            IM_COL32(0x00, 0x58, 0x44, 0xFF),  // 950  teal    #005844
            IM_COL32(0x04, 0x57, 0x2D, 0xFF),  // 1000 green   #04572d
            IM_COL32(0x32, 0x52, 0x10, 0xFF),  // 1050 green   #325210
            IM_COL32(0x4A, 0x4C, 0x00, 0xFF),  // 1100 olive   #4a4c00
            IM_COL32(0x5C, 0x44, 0x00, 0xFF),  // 1150 olive   #5c4400
            IM_COL32(0x69, 0x3C, 0x00, 0xFF),  // 1200 brown   #693c00
            IM_COL32(0x71, 0x34, 0x0A, 0xFF),  // 1250 brown   #71340a
            IM_COL32(0x74, 0x2E, 0x27, 0xFF),  // 1300 maroon  #742e27
            IM_COL32(0x73, 0x2D, 0x3E, 0xFF),  // 1350 maroon  #732d3e
            IM_COL32(0x6D, 0x2E, 0x52, 0xFF),  // 1400 purple  #6d2e52
            IM_COL32(0x62, 0x32, 0x64, 0xFF),  // 1450 purple  #623264
            IM_COL32(0x54, 0x37, 0x72, 0xFF),  // 1500 indigo  #543772
            IM_COL32(0x42, 0x3E, 0x7B, 0xFF),  // 1550 indigo  #423e7b
            IM_COL32(0x2A, 0x45, 0x7D, 0xFF),  // 1600 navy    #2a457d
            IM_COL32(0x00, 0x4C, 0x78, 0xFF),  // 1650 navy    #004c78
            IM_COL32(0x00, 0x52, 0x6E, 0xFF),  // 1700 teal    #00526e
            IM_COL32(0x00, 0x56, 0x5D, 0xFF),  // 1750 teal    #00565d
            IM_COL32(0x00, 0x58, 0x4A, 0xFF),  // 1800 green   #00584a
            IM_COL32(0x00, 0x57, 0x33, 0xFF),  // 1850 green   #005733
            IM_COL32(0x29, 0x54, 0x19, 0xFF),  // 1900 green   #295419
            IM_COL32(0x44, 0x4E, 0x00, 0xFF),  // 1950 olive   #444e00
            IM_COL32(0x57, 0x46, 0x00, 0xFF),  // 2000 olive   #574600
        };
        float pos = 0.0f;
        for (auto& col : heatColor) {
            LineColors.addMark(pos, col);
            pos += (1.f / (heatColor.size() - 1));
        }
        LineColors.refreshCache();
    }
    Shader = std::make_unique<HeatmapShader>();
}

FunscriptHeatmap::FunscriptHeatmap() noexcept
{
    glGenTextures(1, &speedTexture);
    glBindTexture(GL_TEXTURE_2D, speedTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); 
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, SpeedTextureResolution, 1, 0, GL_RED, GL_UNSIGNED_BYTE, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void FunscriptHeatmap::Update(float totalDuration, const FunscriptArray& actions) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    std::vector<float> speedBuffer; 
    speedBuffer.resize(SpeedTextureResolution, 0.f);
    std::vector<uint16_t> sampleCountBuffer;
    sampleCountBuffer.resize(SpeedTextureResolution, 0);

    float timeStep = totalDuration / SpeedTextureResolution;

    for(uint32_t i = 0, j = 1, size = actions.size(); j < size; i = j++)
    {
        auto prev = actions[i];
        auto next = actions[j];

        float strokeDuration = next.atS - prev.atS;
        float speed = std::abs(prev.pos - next.pos) / strokeDuration;
    
        uint32_t prevSampleIdx = prev.atS / timeStep;
        uint32_t nextSampleIdx = next.atS / timeStep;
        if(prevSampleIdx == nextSampleIdx)
        {
            if(prevSampleIdx < SpeedTextureResolution)
            {
                sampleCountBuffer[prevSampleIdx] += 1;
                speedBuffer[prevSampleIdx] += speed;
            }
        }
        else
        {
            if(prevSampleIdx < SpeedTextureResolution && nextSampleIdx < SpeedTextureResolution)
            {
                for(int x = prevSampleIdx; x < nextSampleIdx; x += 1)
                {
                    sampleCountBuffer[x] += 1;
                    speedBuffer[x] += speed;
                }
            }
        }
    }

    for(uint32_t i=0; i < SpeedTextureResolution; i += 1)
    {
        speedBuffer[i] /= sampleCountBuffer[i] > 0 ? (float)sampleCountBuffer[i] : 1.f;
        speedBuffer[i] /= MaxSpeedPerSecond;
        speedBuffer[i] = Util::Clamp(speedBuffer[i], 0.f, 1.f);
    }

    glBindTexture(GL_TEXTURE_2D, speedTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, SpeedTextureResolution, 1, 0, GL_RED, GL_FLOAT, speedBuffer.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

void FunscriptHeatmap::DrawHeatmap(ImDrawList* drawList, const ImVec2& min, const ImVec2& max) noexcept
{
    drawList->AddCallback([](const ImDrawList* parentList, const ImDrawCmd* cmd) noexcept
    {
        auto self = (FunscriptHeatmap*)cmd->UserCallbackData;
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, self->speedTexture);
        glActiveTexture(GL_TEXTURE0);

        auto drawData = OFS_ImGui::CurrentlyRenderedViewport->DrawData;
        float L = drawData->DisplayPos.x;
        float R = drawData->DisplayPos.x + drawData->DisplaySize.x;
        float T = drawData->DisplayPos.y;
        float B = drawData->DisplayPos.y + drawData->DisplaySize.y;
        const float orthoProjection[4][4] =
        {
            { 2.0f / (R - L), 0.0f, 0.0f, 0.0f },
            { 0.0f, 2.0f / (T - B), 0.0f, 0.0f },
            { 0.0f, 0.0f, -1.0f, 0.0f },
            { (R + L) / (L - R),  (T + B) / (B - T),  0.0f,   1.0f },
        };
        Shader->Use();
        Shader->ProjMtx(&orthoProjection[0][0]);
        Shader->SpeedTex(1);

    }, this);
    drawList->AddImage(0, min, max);
    drawList->AddCallback(ImDrawCallback_ResetRenderState, 0);
}

#include "imgui_impl/imgui_impl_opengl3.h"

std::vector<uint8_t> FunscriptHeatmap::RenderToBitmap(int16_t width, int16_t height) noexcept
{
    width = Util::Min(width, FunscriptHeatmap::MaxResolution);
    height = Util::Min(height, FunscriptHeatmap::MaxResolution);

    // Prepare temporary framebuffer
    uint32_t tmpFramebuffer = 0;
    uint32_t tmpColorTex = 0;

    glGenFramebuffers(1, &tmpFramebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, tmpFramebuffer);

    glGenTextures(1, &tmpColorTex);
    glBindTexture(GL_TEXTURE_2D, tmpColorTex);

    glTexImage2D(GL_TEXTURE_2D, 0, OFS_InternalTexFormat, width, height, 0, OFS_TexFormat, GL_UNSIGNED_BYTE, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tmpColorTex, 0);
    GLenum DrawBuffers[1] = { GL_COLOR_ATTACHMENT0 };
    glDrawBuffers(1, DrawBuffers);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        // FIXME: leaking memory
        return {};
    }

    // Backup out main ImGuiContext
    auto prevContext = ImGui::GetCurrentContext();

    // Create a temporary ImGuiContext
    auto tmpContext = ImGui::CreateContext();
    ImGui::SetCurrentContext(tmpContext);
    ImGui_ImplOpenGL3_Init(OFS_SHADER_VERSION);

    // Prepare drawing a single image
    auto& io = ImGui::GetIO();
    io.DisplaySize.x = width;
    io.DisplaySize.y = height;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    // Draw calls
    {
        auto drawList = ImGui::GetForegroundDrawList(ImGui::GetMainViewport());
        DrawHeatmap(drawList, ImVec2(0.f, 0.f), ImVec2(width, height));
    }

    // Render image
    ImGui::Render();
    OFS_ImGui::CurrentlyRenderedViewport = ImGui::GetMainViewport();
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    OFS_ImGui::CurrentlyRenderedViewport = nullptr;


    // Grab the bitmap
    std::vector<uint8_t> bitmap;
    bitmap.resize((size_t)width * (size_t)height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, bitmap.data());

    // Destroy everything
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext(tmpContext);

    glDeleteTextures(1, &tmpColorTex);
    glDeleteFramebuffers(1, &tmpFramebuffer);

    // Reset to default framebuffer and main ImGuiContext    
    ImGui::SetCurrentContext(prevContext);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return bitmap;
}