# OpenFunscripter — 3D Simulator fork

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg?style=flat-square)](https://www.gnu.org/licenses/gpl-3.0)
[![Latest Release](https://img.shields.io/github/v/release/SaekoM/OFS?style=flat-square)](https://github.com/SaekoM/OFS/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/SaekoM/OFS/total?style=flat-square)](https://github.com/SaekoM/OFS/releases)
[![Windows](https://img.shields.io/badge/Windows-0078D6?style=flat-square&logo=windows&logoColor=white)](https://github.com/SaekoM/OFS/releases/latest)
[![Linux](https://img.shields.io/badge/Linux-FCC624?style=flat-square&logo=linux&logoColor=black)](https://github.com/SaekoM/OFS/releases/latest)
[![macOS](https://img.shields.io/badge/macOS-000000?style=flat-square&logo=apple&logoColor=white)](https://github.com/SaekoM/OFS/releases/latest)

A fork of [OpenFunscripter](https://github.com/Eroscripts/OFS) for authoring `.funscript` files (NSFW), adding a **native 3D multi-axis simulator** and an **animated preview exporter**. Built on OpenGL, SDL2, ImGui, libmpv, and [these libraries](./lib).

## Fork features

### 3D multi-axis simulator
- Visualizes the **main stroke plus surge / sway / twist / roll / pitch** on a 3D device (stroker cylinder, opposing twist markers, optional tongue and center indicator).
- Axis mapping follows the **TCode** convention, with **device presets** (All axes / SR6 / OSR2+) that restrict axes to real hardware and label channels (L0–L2, R0–R2).
- **GPU-lit rendering** with orbit camera, per-part colors, an axis gizmo, and a ground grid.
- **Stroke-length line** — a rod from a fixed ground anchor to the cylinder that tilts and stretches with the device, so you can read the travel at a glance.
- **Overlay mode** — detach the simulator into a borderless, movable/resizable window over the video.

### Animated preview export
- Export a **timestamp range** (or a **chapter** from its right-click menu) to **animated WebP** or **AV1 (mp4)**.
- Optionally **composite the simulator onto the video** — either the 3D device or a flat **2D stroke bar** (which reuses your 2D-simulator colors, height lines, and indicators) — positioned with a **draggable live preview**.
- Configurable **resolution / fps / quality**, source **audio** for AV1, and supersampled sim rendering to keep files small.

### Quality-of-life
- **Lua plugin compatibility fixes** — tolerant marshalling for `Checkbox`, `SliderInt`, `InputInt`, `DragInt`.
- **Shift + 1…0** keybinds for the intermediate positions **5, 15, … 95**.

## From upstream (V4)
- Updated color scheme to match ES heatmap coloring.
- Funscript 2.0 & 1.1 format support (full read/write).

![Speed map](./data/speed-map.png)

![OpenFunscripter Screenshot](./OpenFunscripter.jpg)

## Building (contributors / forkers)
1. Clone the repository
2. `cd OFS`
3. `git submodule update --init`
4. Run CMake and compile

Known Linux build dependencies: `build-essential libmpv-dev libglvnd-dev`.

> **AV1 export note:** the animated preview exporter uses `libsvtav1`, which is only in the ffmpeg **"full"** build (not "essentials"). OFS downloads the full build on first use.

## Credits
Fork of [OpenFunscripter](https://github.com/Eroscripts/OFS) (Eroscripts), originally by [OpenFunscripter/OFS](https://github.com/OpenFunscripter/OFS). GPLv3.
