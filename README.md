<div align="center">

<img src="https://github.com/user-attachments/assets/552b8ca0-ebca-4876-9c6a-df38c468d41e" width="120"/>

# Lumina Game Engine

**A modern, high-performance game engine built with Vulkan**

[![License](https://img.shields.io/github/license/mrdrelliot/lumina)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows-blue)](https://github.com/mrdrelliot/lumina)
[![C++](https://img.shields.io/badge/C++-23-blue)](https://github.com/mrdrelliot/lumina)
[![Vulkan](https://img.shields.io/badge/Vulkan-renderer-red)](https://www.vulkan.org/)
[![Discord](https://img.shields.io/discord/1193738186892005387?label=Discord&logo=discord)](https://discord.gg/xQSB7CRzQE)

[Website](https://luminagameengine.com) &bull; [Discord](https://discord.gg/xQSB7CRzQE) &bull; [Documentation](https://luminagameengine.com/getting-started/introduction/)

</div>

---

## Contents

- [Lumina Game Engine](#lumina-game-engine)
  - [Contents](#contents)
  - [Overview](#overview)
  - [Features](#features)
    - [Rendering](#rendering)
    - [Architecture](#architecture)
    - [Editor](#editor)
    - [Performance](#performance)
    - [C# Scripting](#c-scripting)
  - [Documentation](#documentation)
  - [Screenshots](#screenshots)
  - [Getting Started](#getting-started)
    - [Requirements](#requirements)
      - [Windows](#windows)
      - [Linux](#linux)
    - [Installation (Windows)](#installation-windows)
    - [Installation (Linux)](#installation-linux)
    - [Build Configuration](#build-configuration)
    - [Troubleshooting](#troubleshooting)
  - [Contributing](#contributing)
    - [Workflow](#workflow)
    - [Requirements](#requirements-1)
  - [Third-Party Dependencies](#third-party-dependencies)
  - [Acknowledgments](#acknowledgments)
  - [License](#license)
  - [Connect](#connect)

---

## Overview

Lumina is a modern C++ game engine built from the ground up with Vulkan. It is
designed for learning and experimentation with real-world engine architecture,
demonstrating professional design patterns including a reflection system, an
ECS-based gameplay layer, and modern rendering techniques.

> [!NOTE]
> For more detailed information, go to: https://luminagameengine.com

It is well suited for:

- Learning modern game engine architecture
- Experimenting with Vulkan rendering techniques
- Building prototypes on a clean, modular codebase
- Understanding how engines such as Unreal and Godot work internally

Contributions are recognized in several ways, including Steam keys for popular
games and public acknowledgment in the Discord community. Lumina improves
through the work of motivated contributors who help push the engine forward.

> [!CAUTION]
> Lumina is an educational project under active development. APIs may change,
> and some features are experimental. If you encounter build issues, please
> reach out on [Discord](https://discord.gg/xQSB7CRzQE) for assistance.


> [!NOTE]
>
> ### AI Usage
>
> AI has been used selectively for tasks such as static analysis, research, build tooling, and scripting assistance.
>
> The engine itself has been hand-crafted over the course of three years as a passion project. AI is treated as a tool where it provides value, nothing more, nothing less.


---

## Features

### Rendering

- Vulkan-powered renderer with automatic resource tracking and barrier placement
- Forward+ pipeline with clustered lighting for efficient multi-light scenes
- PBR materials authored through a material graph compiled to shader code

### Architecture

- In-house sparse-set Entity Component System with per-type storage layouts, typed views, and lifecycle signals
- Reflection system driving automatic serialization and editor integration
- Modular design with clean separation of concerns

### Editor

- ImGui-based editor with real-time scene manipulation
- Scene hierarchy for entity management
- Component inspector with UI generated automatically via reflection

### Performance

- Multi-threaded task system built with fibers.
- Custom memory allocators built on RPMalloc
- Built-in profiling through Tracy integration

### C# Scripting

- C# scripting via LuminaSharp, a CoreCLR host that bundles the .NET 10 runtime
- EntityScript-based gameplay with full ECS access: `Registry.View` queries,
  C# entity systems, and component reads and writes
- Reflection-driven C++ to C# bindings generated automatically by the Reflector
- Hot-reloadable scripts for iterating on gameplay without recompiling
- High-level gameplay APIs through the `World` facade: Physics, Navigation,
  Input, Messages, and GameplayTags

---

## Documentation

[Getting Started](https://luminagameengine.com/getting-started/introduction/)

---

## Screenshots

<div align="center">
  <img width="800" alt="Lumina editor" src="https://github.com/user-attachments/assets/ba4fc531-362c-4b4f-bbec-261bb1e79652" />
  <img width="800" alt="Lumina scene" src="https://github.com/user-attachments/assets/5df3dee0-71fe-4439-b851-5e22ff2b23cc" />
</div>

<details>
  <summary><b>Show more screenshots</b></summary>

  <div align="center">
    <img width="800" alt="Lumina screenshot" src="https://github.com/user-attachments/assets/086a1e5e-4d8d-4c98-be81-bc77287fba39" />
    <img width="800" alt="Lumina screenshot" src="https://github.com/user-attachments/assets/66d739ca-f257-4350-b2f0-2e10d66f5591" />
    <img width="800" alt="Lumina screenshot" src="https://github.com/user-attachments/assets/68e86207-20f2-4200-8649-257738b2855f" />
    <img width="800" alt="Lumina screenshot" src="https://github.com/user-attachments/assets/a4c7c497-d975-47d2-a695-5342e11e8f44" />
    <img width="800" alt="Lumina screenshot" src="https://github.com/user-attachments/assets/5343eff8-545a-47ca-a858-b2aa4f1fef71" />
  </div>
</details>

https://github.com/user-attachments/assets/3d797479-fc47-4b8f-baf4-87315709d0c2

---

## Getting Started

### Requirements

**Every platform** needs a GPU that supports Vulkan mesh shaders
(`VK_EXT_mesh_shader`): NVIDIA Turing (GTX 16-series / RTX 20-series) or newer,
AMD RDNA2 (RX 6000) or newer, or Intel Arc. The renderer's geometry pipeline is
built on them, so the editor refuses to start on anything older and says so.

#### Windows

- Windows 10 (1803 or newer) or Windows 11, 64-bit
- **Visual Studio 2026 (18.0 or newer)** with the MSVC v143 toolset and the
  ".NET desktop development" workload
- **.NET 10 SDK** (x64)

> [!IMPORTANT]
> The engine's C# layer (LuminaSharp) targets **`net10.0`**. Only Visual Studio
> **18.0+ (2026)** can build that target, VS 2022 (17.x) fails with
> `error NETSDK1209` even if you install the standalone .NET 10 SDK, because VS
> uses its own bundled MSBuild. `Setup.bat` validates this for you and stops with
> a clear message if anything is missing.

#### Linux

- A 64-bit distribution
- **GCC 13 or newer**, or Clang against a libstdc++ that new. The tree uses
  `<format>`, which is where that floor comes from. Built and tested with GCC
  13-15; newer pre-release compilers tend to reject vendored third-party code
  for reasons that are not bugs in this engine.
- **.NET 10 SDK**
- X11 development packages, which GLFW links directly
- A Vulkan **loader and driver** at run time. These are not needed to compile:
  the Vulkan headers are vendored and volk resolves entry points with `dlopen`,
  so a machine with no driver builds a working editor it cannot launch.

  On Debian or Ubuntu:

  ```bash
  # build
  sudo apt-get install -y g++-13 pkg-config libx11-dev libxrandr-dev \
      libxinerama-dev libxcursor-dev libxi-dev libxkbcommon-dev
  # run
  sudo apt-get install -y libvulkan1 mesa-vulkan-drivers vulkan-tools
  # .NET 10 SDK: https://dotnet.microsoft.com/download/dotnet/10.0
  ```

  `Setup.sh` checks all of this and tells you what is missing, including
  whether any installed GPU actually reports `VK_EXT_mesh_shader`.

> [!NOTE]
> No IDE is required. On Windows,
> [JetBrains Rider](https://www.jetbrains.com/rider/) and Visual Studio both open
> the generated solution. On Linux there is no solution:
> [CLion](https://www.jetbrains.com/clion/) reads the generated
> `compile_commands.json` for C++, and Rider opens the engine's C# projects.

### Installation (Windows)

1. **Clone the repository**

   ```bash
   git clone https://github.com/mrdrelliot/luminaengine
   cd LuminaEngine
   ```

2. **Run setup**

   ```bash
   Setup.bat
   ```
   It downloads one prebuilt dependency bundle
   (`External-Win64.zip`, ~192 MB), verifies it against a SHA-256 hash pinned in this
   repo (once the maintainer records it, see
   [`DEPENDENCIES.md`](DEPENDENCIES.md)), persists the `LUMINA_DIR` environment
   variable, configures git hooks, and generates `Lumina.sln`.

   The bundle contains the .NET 10 runtime (C# scripting), LLVM/Clang 19
   (reflection codegen), the Slang shader compiler, RenderDoc, and Tracy. Each
   is open source; [`DEPENDENCIES.md`](DEPENDENCIES.md) lists every component
   with its version, purpose, upstream source, and license, and explains how to
   fetch them yourself instead of using the bundle.

   To skip the prompt for unattended/CI runs, pass `--yes` (or set
   `LUMINA_SETUP_YES=1`). If the download fails, manually download
   [External-Win64.zip](https://github.com/MrDrElliot/LuminaEngine/releases/download/external-deps/External-Win64.zip),
   extract it into the `LuminaEngine/` folder, then run
   `GenerateProjectFiles.bat`.

1. **Open the solution**

   Open `Lumina.sln` in Visual Studio.

2. **Build and run**

   - Set `Lumina` as the startup project.
   - Pick a configuration: `Development Editor` (default), `Debug Editor` for
     full debugger functionality at the cost of speed, or `Development Game` for
     the runtime without the editor.
   - Press F5, or use **Build -> Run**.

3. **Start developing**

   - Copy `Templates/Blank/` to create a new project, then run its
     `GenerateProject.bat` to produce a solution.

### Installation (Linux)

Every script below has a `.bat` counterpart of the same name on Windows, and
takes the same arguments.

1. **Clone the repository**

   ```bash
   git clone https://github.com/mrdrelliot/luminaengine
   cd LuminaEngine
   ```

2. **Run setup**

   ```bash
   ./Setup.sh
   ```

   It checks the prerequisites above, downloads one prebuilt dependency bundle
   (`External-Linux64.tar.gz`, ~186 MB), verifies it against a SHA-256 hash
   pinned in this repo, configures git hooks, and offers to add `LUMINA_DIR` to
   your shell profile.

   The bundle contains the .NET 10 runtime (C# scripting), LLVM/Clang 19
   (reflection codegen), the Slang shader compiler, RenderDoc, and Tracy. Each
   is open source; [`DEPENDENCIES.md`](DEPENDENCIES.md) lists every component
   with its version, purpose, upstream source, and license, and explains how to
   fetch them yourself instead of using the bundle.

   To skip the prompts for unattended/CI runs, pass `-Yes` (or set
   `LUMINA_SETUP_YES=1`); an unattended run never edits your shell profile. If
   the download fails, fetch
   [External-Linux64.tar.gz](https://github.com/MrDrElliot/LuminaEngine/releases/download/external-deps/External-Linux64.tar.gz)
   by hand and extract it into the `LuminaEngine/` folder.

3. **Build**

   ```bash
   ./LuminaBuild.sh Build Lumina -TargetType=Editor
   ```

   The Reflector is a prerequisite of that target and builds itself first, so
   this is the only command you need. Add `-Configuration=Debug` for full
   debugger fidelity at the cost of speed, or `-TargetType=Game` for the runtime
   without the editor.

4. **Run**

   ```bash
   ./LuminaBuild.sh Run Lumina -TargetType=Editor
   ```

   `Run` locates the binary through the same rules the build used, so it always
   launches the configuration you name rather than whatever was built last. It
   takes the same `-TargetType` and `-Configuration` flags as `Build`, and
   anything after a bare `--` is forwarded to the editor untouched. The
   executable itself is at `Binaries/Linux64/Lumina-Editor-Development` if you would
   rather launch it directly; it finds the engine relative to itself, so it does
   not care what directory you run it from.

5. **Set up code completion**

   ```bash
   ./GenerateProjectFiles.sh
   ```

   This writes `compile_commands.json` at the repository root. No solution is
   generated on Linux, because nothing here can open one. Point your editor at
   the repository root and it will pick the database up:

   - **clangd** (and the VS Code clangd extension) finds it with no
     configuration.
   - **CLion**: open the folder and select the compile database when prompted.

   Rider is the tool for the engine's C# projects (`LuminaBuildTool`,
   `LuminaSharp`) rather than for the compile database.

   Re-run it after adding or removing modules, plugins or sources. Ordinary
   source files are discovered at build time, and a build that changes a
   `Build.cs` refreshes the database on its own.

6. **Start developing**

   Create a project from the editor, or copy `Templates/Blank/` by hand and run
   its `GenerateProject.sh`. Every generated project ships both
   `GenerateProject.bat` and `GenerateProject.sh`, so a project created on one
   platform still works on the other.

### Build Configuration

Optional engine features are controlled from a single file,
[`Engine/Build/BuildConfiguration.json`](Engine/Build/BuildConfiguration.json). Each feature is
`"auto"`, `"on"` (force into every configuration) or `"off"` (strip from every
configuration), and the choices are baked in when you regenerate the solution.

| Feature | What it controls | `"auto"` default |
| -------------------- | ------------------------------------------ | ---------------------------------------- |
| `Tracy` | Tracy CPU/GPU profiler (`LUMINA_PROFILE_*`) | Debug + Development; **off** in Shipping |
| `GpuProfiling` | GPU timing query pools and their readback | Debug + Development; **off** in Shipping |
| `Validation` | Vulkan validation + sync layers | Debug only |
| `GpuValidation` | GPU-assisted validation inside every shader | **off** everywhere; `--gpuvalidation` toggles at runtime |
| `Aftermath` | NVIDIA Nsight Aftermath GPU crash dumps | NVIDIA hosts, Debug + Development |
| `RadeonGpuDetective` | AMD object naming and fault reporting for `.rgd` dumps | AMD hosts, Debug + Development |
| `VerboseLogging` | `LOG_TRACE` / `LOG_DEBUG` / `LOG_INFO` macros | Debug + Development; **off** in Shipping |
| `BugSplat` | Crash and minidump upload to the engine's database | Editor targets on Windows only |
| `Box3DDebugChecks` | Box3D's own asserts and internal validation | Debug only |

`"off"` is a true strip: e.g. `Tracy = "off"` drops the Tracy library from the
build entirely and turns every profiling macro into a no-op, and
`VerboseLogging = "off"` removes `LOG_TRACE/DEBUG/INFO` (warnings and errors are
always kept). `Aftermath` auto-enables only when an NVIDIA GPU is detected on the
machine generating the solution.

For a one-off build (e.g. a profiling-free package) you can override any feature
on the command line without editing the file, the flag wins:

```bash
GenerateProjectFiles.bat --tracy=off --validation=on --verbose-logging=off
```

On Linux there is no solution to bake the choices into, so pass the same flags
to the build (or to `./GenerateProjectFiles.sh`, which only affects the compile
database):

```bash
./LuminaBuild.sh Build Lumina -TargetType=Editor -Tracy=off -Validation=on
```

Regenerating prints the resolved feature set, e.g.
`[Lumina] Build features: Tracy=auto (Debug, Development)  Validation=auto (Debug)  Aftermath=auto (Debug, Development)  VerboseLogging=auto (Debug, Development)  [NVIDIA GPU]`.

### Troubleshooting

#### Linux

> [!TIP]
> - **"Vulkan Device Unsuitable: No GPU meeting the renderer's requirements was
>   found"?** The log names each GPU it rejected and why. If the reason is
>   `no VK_EXT_mesh_shader` for every device, the hardware is below the floor in
>   [Requirements](#requirements). If a GPU you expected is missing from the list
>   entirely, the Vulkan loader is not seeing its driver; check
>   `vulkaninfo --summary` and compare.
> - **Hybrid-graphics laptop starting on the wrong GPU?** The device is chosen at
>   startup and the discrete one is preferred when it enumerates at all. If it
>   does not appear in `vulkaninfo --summary`, the proprietary driver is not
>   loaded, and no engine setting can work around that.
> - **`error: the .NET SDK is required`, but you installed it?**
>   `dotnet-install.sh` puts it in `$HOME/.dotnet` and persists nothing. The
>   scripts here look there anyway, but your own shell will not until you add it
>   to `PATH`.
> - **The build uses fewer cores than the machine has?** Deliberate: parallelism
>   is capped to fit available memory, and the reason is logged. Override with
>   `-MaxParallel=<n>`.
> - **Link failure on `stdc++_libbacktrace` or `stdc++exp`?** The archive behind
>   `std::stacktrace` was renamed in GCC 14 and the toolchain probes for
>   whichever one your compiler ships. If it fails anyway, the compiler is
>   probably not the one you think it is; check `CXX`.
> - **A build failure inside `External/` or `ThirdParty/`?** Usually a compiler
>   newer than the tree has been built with. Pin a stable one:
>   `sudo apt-get install -y g++-15 && export CXX=g++-15 CC=gcc-15`.

#### Windows

> [!TIP]
> - **`error NETSDK1209` / "does not support targeting .NET 10.0"?** Your Visual
>   Studio is older than 18.0 (2026). Install VS 2026, the standalone .NET 10
>   SDK alone will **not** fix this, because VS builds with its own bundled
>   MSBuild. Run `Setup.bat` (or `BuildScripts\CheckPrerequisites.ps1`) to verify.
> - **Missing v143 toolset?** Install it via Visual Studio Installer ->
>   Individual Components -> MSVC v143 Build Tools.
> - **"Cannot find .generated.h" error?** Build again; Visual Studio sometimes
>   needs a second pass to pick up generated files.
> - **C1076 compiler limit reached?** Retry the build; this is a known
>   intermittent issue with a font file.
> - **"Application control policy blocked this file"?** Disable Windows 11
>   Smart App Control.
> - **`error NETSDK1004` / "Assets file ... project.assets.json not found"?** The
>   managed projects' NuGet restore lives under `Intermediates/`, so deleting that
>   folder removes it. Run `GenerateProjectFiles.bat` (it restores at the end), or
>   pass `-restore` to MSBuild.
> - **"C# scripting disabled: managed bootstrap missing"?** The `LuminaSharp`
>   managed project didn't build, usually the restore above. Run
>   `GenerateProjectFiles.bat` and rebuild (the `Lumina` app builds it as a
>   dependency). From the command line, pass `-restore` to MSBuild.

**Still failing on either platform?**
[Submit an issue](https://github.com/mrdrelliot/LuminaEngine/issues) or reach
out on Discord.

> [!NOTE]
> Standalone game projects locate the engine through `LUMINA_DIR`. `Setup.bat`
> persists it via `setx`; `Setup.sh` offers to add it to your shell profile,
> since Linux has no equivalent user environment store. To set it manually:
> ```bash
> setx LUMINA_DIR "C:\path\to\lumina"          # Windows
> export LUMINA_DIR="/path/to/lumina"          # Linux, in your shell profile
> ```

> [!CAUTION]
> After pulling or merging, delete `Binaries/` and `Intermediates/`, then run
> `GenerateProjectFiles.bat` on Windows to regenerate the solution, or
> `./GenerateProjectFiles.sh` on Linux to refresh the compile database.

---

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for the complete guidelines.

Contributions are welcome, whether they are bug fixes, features, or
documentation improvements.

### Workflow

1. Fork the repository.
2. Create a feature branch: `git checkout -b feature/amazing-feature`.
3. Make your changes following the [coding standards](CONTRIBUTING.md).
4. Add tests where applicable.
5. Commit with clear messages: `git commit -m "Add amazing feature"`.
6. Push to your branch: `git push origin feature/amazing-feature`.
7. Open a pull request.

### Requirements

- Clean, well-documented code
- Adherence to existing architecture patterns
- Tests where appropriate
- Updated documentation as needed

---

## Third-Party Dependencies

Listed alphabetically.

| Library | Purpose |
| --------- | --------- |
| basis_universal | GPU texture compression with runtime transcoding to BC7/ETC/ASTC |
| Box3D | Multi-threaded rigid body physics with a stateless capsule character mover |
| BugSplat | Crash and minidump upload to the engine's crash database |
| cgltf | glTF 2.0 parser behind the mesh and material importers |
| ConcurrentQueue | Lock-free queue supporting multiple producers and consumers |
| DPDK | Source of the vectorized memcpy in `Engine/Source/Runtime/Source/Memory/Memcpy.h`; vendored for its license only |
| ENet | Reliable UDP transport behind the networking plugin |
| FreeType | Font rasterization for RmlUi, MSDFGen, and editor text |
| GLFW | Multi-platform window and input library for OpenGL and Vulkan |
| GoogleTest | C++ testing framework with assertions, fixtures, and test discovery |
| ImGui | Immediate-mode GUI for rapid tool development |
| MeshOptimizer | Mesh optimization for vertex cache, overdraw, and buffer compression |
| MikkTSpace | Reference tangent-space generation for imported meshes |
| Miniaudio | Single-file audio playback and capture library |
| Miniz | Deflate compression for package and pak archive serialization |
| MSDFGen | Multi-channel signed distance fields for crisp world-space text |
| .NET Runtime | CoreCLR (.NET 10) bundled as the host for C# (LuminaSharp) scripting |
| Nlohmann JSON | Modern JSON library with STL compatibility |
| NVIDIA Aftermath | GPU crash debugging and post-mortem dump analysis |
| NVIDIA Nsight Perf | GPU counter collection behind the hardware profiler plugin |
| Recast & Detour | Navigation mesh generation and pathfinding |
| RenderDoc | Graphics debugger integration for frame capture and analysis |
| RmlUi | Retained HTML/CSS-style UI used for game and editor interfaces |
| RPMalloc | Lock-free, thread-caching memory allocator |
| Slang | Modern shader language and compiler with SPIR-V / HLSL output |
| stb_image | Single-header image loading library |
| TinyObjLoader | Lightweight OBJ parser with MTL material support |
| Tracy | Real-time frame profiler with sampling, GPU zones, and lock contention tracking |
| ufbx | FBX loader for geometry, skeletons, and animation |
| Volk | Vulkan meta-loader for runtime function loading |
| Vulkan | Low-level graphics API providing explicit GPU control |
| VulkanMemoryAllocator | Memory management library for Vulkan with defragmentation |
| xxHash | Extremely fast non-cryptographic hash algorithm |

---

## Acknowledgments

Lumina is inspired by and learns from these open-source engines:

- [Spartan Engine](https://github.com/PanosK92/SpartanEngine) - Feature-rich Vulkan engine
- [Kohi Game Engine](https://kohiengine.com/) - Educational engine series
- [Lumix Game Engine](https://github.com/nem0/LumixEngine) - Fully working indie engine
- [ezEngine](https://ezengine.net/) - Professional open-source engine
- [Godot](https://godotengine.org/) - High-quality open-source engine
- [Unreal Engine](https://www.unrealengine.com/) - Industry-standard engine

Thanks to the game engine development community for sharing knowledge and
resources.

---

## License

Lumina is licensed under the [Apache 2.0 License](LICENSE).

```
Copyright 2024 Dr. Elliot

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```

---

## Connect

- Blog: [dr-elliot.com](https://www.dr-elliot.com)
- Discord: [Join the community](https://discord.gg/xQSB7CRzQE)
- GitHub: [mrdrelliot/lumina](https://github.com/mrdrelliot/lumina)
