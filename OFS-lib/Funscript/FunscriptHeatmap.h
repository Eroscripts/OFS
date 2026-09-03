#pragma once
#include "GradientBar.h"
#include "Funscript.h"

#include "imgui.h"

#include <utility>
#include <vector>

class FunscriptHeatmap
{
public:
	static constexpr float MaxSpeedPerSecond = 2000.f;
	static constexpr int16_t MaxResolution = 4096;

	static ImGradient LineColors;

	static void Init() noexcept;

	// Built-in action-line palettes. 0 = Fork (detailed 41-stop), 1 = Classic (OFS).
	enum LineProfile : int32_t { Fork = 0, Classic = 1, Custom = 2 };
	static std::vector<std::pair<float, ImU32>> LinePreset(int32_t profile) noexcept;
	// Rebuild LineColors from an ordered list of (pos 0..1, RGBA) stops.
	static void SetLineColors(const std::vector<std::pair<float, ImU32>>& stops) noexcept;

	uint32_t speedTexture = 0;

	FunscriptHeatmap() noexcept;

	void DrawHeatmap(ImDrawList* drawList, const ImVec2& min, const ImVec2& max) noexcept;
	void Update(float totalDuration , const FunscriptArray& actions) noexcept;

	std::vector<uint8_t> RenderToBitmap(int16_t width, int16_t height) noexcept;
};
