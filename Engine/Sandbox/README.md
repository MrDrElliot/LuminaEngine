# Sandbox

A Lumina Engine project.

## Requirements

- The `LUMINA_DIR` environment variable must point at your engine install (set by the engine's `Setup.bat`).
- Visual Studio 2026 (18.0+) with the C++ workload. (The engine's C# layer targets `net10.0`, which needs VS 18.0+; the standalone .NET 10 SDK alone is not enough.)

## First-time setup

1. Run `GenerateProject.bat` from this folder. This invokes `%LUMINA_DIR%\Tools\premake5.exe vs2026` and writes `Sandbox.slnx`.
2. Open `Sandbox.slnx` in **Visual Studio** or **JetBrains Rider**.
3. Press **F5**.

F5 builds the game DLL (`Binaries\Windows64\Sandbox-Development.dll`) and launches the Lumina editor with this project pre-loaded. Breakpoints in your game module will hit as soon as `IMPLEMENT_MODULE` runs.

- **Visual Studio** uses the `.vcxproj.user` settings premake writes (`debugcommand` -> `Lumina-Development.exe`).
- **Rider** uses the shared `.run/` launch configurations that ship with this template. Pick one from the configuration dropdown next to the Run button.

## Scripting (C#)

Gameplay is written in **C#** with `LuminaSharp`. Scripts live in `Game/Scripts/` and are **compiled inside the editor**, edit a `.cs`, save, and the change is live; no DLL rebuild, no editor restart.

- A script is a class deriving from `EntityScript` (see `Game/Scripts/ExampleScript.cs`). It gets `Entity`, `World`, `Registry`, a cached `Transform`, and lifecycle hooks (`OnAttach` / `OnReady` / `OnUpdate` / `OnDetach`) plus input and collision callbacks.
- Attach a script to an entity by adding a **C# Script** component and selecting the script class. Fields marked `[Property]` show up in the inspector.
- `Game/Scripts/<...>.Scripts.csproj` is **generated** for IDE IntelliSense only (it references the engine's `LuminaSharp.dll`). It is recreated on project load and via the `dotnet.genprojects` console command, never commit it, never rely on it for the build (the engine compiles scripts itself at runtime).

The C++ module in `Source/` is optional: use it for native types, custom components, and engine integrations. A project can be pure C# scripts on top of it.

## Iterating

- **C# scripts** (`Game/Scripts/*.cs`): save in your editor; the running engine recompiles and reloads them.
- **Content** (assets in `Game/Content/`): hot-reloads inside the editor; no rebuild needed.
- **C++** (`Source/*.cpp` / `*.h`): press F5 again, VS rebuilds the DLL and relaunches the editor. New `.h` / `.cpp` files require a re-run of `GenerateProject.bat` (premake globs sources at generate time).

## Project layout

```
Sandbox.lproject               Project descriptor (name, GUID, plugins)
premake5.lua                   Build script, calls LuminaGameProject()
GenerateProject.bat            One-shot: regenerate the .slnx after source changes
Config/GameSettings.json       Per-project engine settings (startup maps, cook roots, ...)
Source/                        Your C++ module (optional)
Game/Content/                  Assets, surfaced to the engine under /Game/Content
Game/Scripts/                  C# scripts, compiled in-editor (surfaced under /Game/Scripts)
Tools/ReflectionRunner.bat     Prebuild step, regenerates C++ reflection code
Intermediates/Reflection/      Generated reflection code (do not edit)
```
