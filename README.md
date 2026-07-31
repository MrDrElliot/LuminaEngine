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
    - [Installation](#installation)
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

- Entity Component System (ECS) built on EnTT for high-performance gameplay code
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

> [!NOTE]
> [JetBrains Rider](https://www.jetbrains.com/rider/) is the recommended IDE for
> Lumina development, but it is not required.

### Installation

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
   (`External.zip`, ~671 MB), verifies it against a SHA-256 hash pinned in this
   repo (once the maintainer records it, see
   [`DEPENDENCIES.md`](DEPENDENCIES.md)), persists the `LUMINA_DIR` environment
   variable, configures git hooks, and generates `Lumina.slnx`.

   The bundle contains the .NET 10 runtime (C# scripting), LLVM/Clang 19
   (reflection codegen), the Slang shader compiler, RenderDoc, and Tracy. Each
   is open source; [`DEPENDENCIES.md`](DEPENDENCIES.md) lists every component
   with its version, purpose, upstream source, and license, and explains how to
   fetch them yourself instead of using the bundle.

   To skip the prompt for unattended/CI runs, pass `--yes` (or set
   `LUMINA_SETUP_YES=1`). If the download fails, manually download
   [External.zip](https://github.com/MrDrElliot/LuminaEngine/releases/download/external-deps/External.zip),
   extract it into the `LuminaEngine/` folder, then run
   `GenerateProjectFiles.bat`.

1. **Open the solution**

   Open `Lumina.slnx` in Visual Studio.

2. **Build and run**

   - Set `Lumina` as the startup project.
   - Choose the `Development` or `Debug` configuration. Debug is significantly
     slower but enables full debugger functionality.
   - Select a platform: `Editor` (default, includes editor tooling) or `Game`
     (runtime only, no editor).
   - Press F5, or use **Build -> Run**.

3. **Start developing**

   - Open the `Sandbox` project to experiment.
   - Or copy `Templates/Blank/` to create a new project, then run its
     `GenerateProject.bat` to produce a solution.

### Build Configuration

Optional engine features are controlled from a single file,
[`BuildScripts/BuildConfig.lua`](BuildScripts/BuildConfig.lua). Each feature is
`"auto"`, `"on"` (force into every configuration) or `"off"` (strip from every
configuration), and the choices are baked in when you regenerate the solution.

| Feature      | What it controls                          | `"auto"` default                         |
| ------------ | ----------------------------------------- | ---------------------------------------- |
| `Tracy`          | Tracy CPU/GPU profiler (`LUMINA_PROFILE_*`)        | Debug + Development; **off** in Shipping |
| `Validation`     | Vulkan validation + sync layers                   | Debug only                               |
| `Aftermath`      | NVIDIA Nsight Aftermath GPU crash dumps            | NVIDIA machines, Debug + Development      |
| `VerboseLogging` | `LOG_TRACE` / `LOG_DEBUG` / `LOG_INFO` macros      | Debug + Development; **off** in Shipping |

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

Regenerating prints the resolved feature set, e.g.
`[Lumina] Build features: Tracy=auto (Debug, Development)  Validation=auto (Debug)  Aftermath=auto (Debug, Development)  VerboseLogging=auto (Debug, Development)  [NVIDIA GPU]`.

### Troubleshooting

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
> - **Build still failing?**
>   [Submit an issue](https://github.com/mrdrelliot/LuminaEngine/issues) or
>   reach out on Discord.

> [!NOTE]
> `Setup.bat` persists `LUMINA_DIR` automatically via `setx`. To set it
> manually:
> ```bash
> setx LUMINA_DIR "C:\path\to\lumina"
> ```

> [!CAUTION]
> After pulling or merging, delete `Binaries/` and `Intermediates/` and run
> `GenerateProjectFiles.bat` to regenerate the solution.

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
|---------|---------|
| basis_universal | GPU texture compression with runtime transcoding to BC7/ETC/ASTC |
| ConcurrentQueue | Lock-free queue supporting multiple producers and consumers |
| EASTL | EA Standard Template Library optimized for game development |
| EnTT | Fast entity component system with sparse-set storage and signals |
| FastGLTF | High-performance glTF 2.0 parser with full specification support |
| GLFW | Multi-platform window and input library for OpenGL and Vulkan |
| GoogleTest | C++ testing framework with assertions, fixtures, and test discovery |
| ImGui | Immediate-mode GUI for rapid tool development |
| JoltPhysics | Multi-threaded physics engine with continuous collision detection |
| MeshOptimizer | Mesh optimization for vertex cache, overdraw, and buffer compression |
| Miniaudio | Single-file audio playback and capture library |
| .NET Runtime | CoreCLR (.NET 10) bundled as the host for C# (LuminaSharp) scripting |
| Nlohmann JSON | Modern JSON library with STL compatibility |
| NVIDIA Aftermath | GPU crash debugging and post-mortem dump analysis |
| OpenFBX | Lightweight FBX loader for geometry, skeletons, and animation |
| RenderDoc | Graphics debugger integration for frame capture and analysis |
| RPMalloc | Lock-free, thread-caching memory allocator |
| Slang | Modern shader language and compiler with SPIR-V / HLSL output |
| stb_image | Single-header image loading library |
| TinyObjLoader | Lightweight OBJ parser with MTL material support |
| Tracy | Real-time frame profiler with sampling, GPU zones, and lock contention tracking |
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
