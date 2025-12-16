# OpenFunscripter

[![Latest Release](https://img.shields.io/github/v/release/Eroscripts/OFS?style=flat-square)](https://github.com/Eroscripts/OFS/releases/latest)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg?style=flat-square)](https://www.gnu.org/licenses/gpl-3.0)
[![Downloads](https://img.shields.io/github/downloads/Eroscripts/OFS/total?style=flat-square)](https://github.com/Eroscripts/OFS/releases)
[![Windows](https://img.shields.io/badge/Windows-0078D6?style=flat-square&logo=windows&logoColor=white)](https://github.com/Eroscripts/OFS/releases/latest)
[![Linux](https://img.shields.io/badge/Linux-FCC624?style=flat-square&logo=linux&logoColor=black)](https://github.com/Eroscripts/OFS/releases/latest)
[![macOS](https://img.shields.io/badge/macOS-000000?style=flat-square&logo=apple&logoColor=white)](https://github.com/Eroscripts/OFS/releases/latest)

Can be used to create `.funscript` files. (NSFW)  
The project is based on OpenGL, SDL2, ImGui, libmpv, & all these other great [libraries](https://github.com/Eroscripts/OFS/tree/master/lib).

### V4 Features

- **Updated color scheme** — Match ES heatmap coloring
- **Funscript 2.0 & 1.1 format support** — Full read/write support for the latest funscript specifications

![Speed map](./data/speed-map.png)

![OpenFunscripter Screenshot](./OpenFunscripter.jpg)

### How to build ( for people who want to contribute or fork )
1. Clone the repository
2. `cd "OpenFunscripter"`
3. `git submodule update --init`
4. Run CMake and compile

Known linux dependencies to just compile are `build-essential libmpv-dev libglvnd-dev`.  
