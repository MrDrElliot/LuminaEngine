# $PROJECTNAME

A Lumina Engine project.

## Requirements

- The `LUMINA_DIR` environment variable must point at your engine install (set up by the engine's `Setup.bat` on Windows, `Setup.sh` on Linux).
- **Windows:** Visual Studio 2026 (18.0+) with the C++ workload. (The engine's C# layer targets `net10.0`, which needs VS 18.0+; the standalone .NET 10 SDK alone is not enough.)
- **Linux:** GCC 13+ and the .NET 10 SDK. See the engine's README for the full package list.

## First-time setup (Windows)

1. Run `GenerateProject.bat` from this folder. It calls `%LUMINA_DIR%\LuminaBuild.bat GenerateProjectFiles` for this project and writes `$PROJECTNAME.sln`.
2. Open `$PROJECTNAME.sln` in **Visual Studio** or **JetBrains Rider**.
3. Press **F5**.

F5 builds the game DLL (`Binaries\Windows64\$PROJECTNAME-Development.dll`) and launches the Lumina
editor with this project pre-loaded. Breakpoints in your game module hit as soon as
`IMPLEMENT_MODULE` runs.

## First-time setup (Linux)

```bash
./GenerateProject.sh                                              # writes compile_commands.json
"$LUMINA_DIR/LuminaBuild.sh" Build $PROJECTNAME -TargetType=Editor
"$LUMINA_DIR/LuminaBuild.sh" Run   $PROJECTNAME -TargetType=Editor
```

That builds the game shared library (`Binaries/Linux64/lib$PROJECTNAME-Development.so`) and launches
the editor with this project pre-loaded. There is no solution on Linux: `GenerateProject.sh` writes
a `compile_commands.json`, which clangd, CLion and the VS Code clangd extension read for completion.

The solution also contains the engine's own targets, because the engine is built from source
alongside your project. Its output stays in the engine tree and is shared by every project, so it
is built once and not again.

**Visual Studio** and **Rider** both read the generated `.vcxproj.user`, which points Run at the
editor with `--Project=` set to this project.

## Scripting (C#)

Gameplay is written in **C#** with `LuminaSharp`. Scripts live in `Game/Scripts/` and are **compiled inside the editor**, edit a `.cs`, save, and the change is live; no DLL rebuild, no editor restart.

- A script is a class deriving from `EntityScript` (see `Game/Scripts/ExampleScript.cs`). It gets `Entity`, `World`, `Registry`, a cached `Transform`, and lifecycle hooks (`OnAttach` / `OnReady` / `OnUpdate` / `OnDetach`) plus input and collision callbacks.
- Attach a script to an entity by adding a **C# Script** component and selecting the script class. Fields marked `[Property]` show up in the inspector.
- `Game/Scripts/<...>.Scripts.csproj` is **generated** for IDE IntelliSense only (it references the engine's `LuminaSharp.dll`). It is recreated on project load and via the `dotnet.genprojects` console command, never commit it, never rely on it for the build (the engine compiles scripts itself at runtime).

The C++ module in `Source/` is optional: use it for native types, custom components, and engine integrations. A project can be pure C# scripts on top of it.

## Iterating

- **C# scripts** (`Game/Scripts/*.cs`): save in your editor; the running engine recompiles and reloads them.
- **Content** (assets in `Game/Content/`): hot-reloads inside the editor; no rebuild needed.
- **C++** (`Source/*.cpp` / `*.h`): press F5 again (or re-run `LuminaBuild.sh Build`) to rebuild the game module and relaunch the editor. New `.h` / `.cpp` files are picked up by the build automatically; re-run `GenerateProject.bat` / `GenerateProject.sh` to make them show up in the IDE's file list.

## Project layout

```
$PROJECTNAME.lproject          Project descriptor (name, GUID, plugins)
GenerateProject.bat            Regenerate the .sln after adding or removing source files (Windows)
GenerateProject.sh             The same, writing compile_commands.json instead (Linux)
Config/GameSettings.json       Per-project engine settings (startup maps, cook roots, ...)
Source/                        Your C++ module (optional)
  $PROJECTNAME.Target.cs       What to build: names the launch module, points Run at the editor
  $PROJECTNAME.Build.cs        How to build it: dependencies, defines, include paths
Plugins/                       Project plugins, one folder each (see Plugins/README.md)
Game/Content/                  Assets, surfaced to the engine under /Game/Content
Game/Scripts/                  C# scripts, compiled in-editor (surfaced under /Game/Scripts)
Logs/                          Engine log for runs with this project loaded (Lumina.log, 5 kept)
CrashDumps/                    Minidumps and GPU crash dumps from runs with this project loaded
```

`Binaries/`, `Intermediates/`, `Logs/` and `CrashDumps/` are generated output and are ignored by git.

## Adding to the C++ module

`Source/$PROJECTNAME.Build.cs` is the whole build configuration for the module. To use another
engine or third-party module, name it there:

```csharp
PrivateDependencyModuleNames.Add("JoltPhysics");
```

Export a type from the module with the `$PROJECTNAMEUPPER_API` macro, as
`Source/$PROJECTNAMEModule.h` does. The build system defines that macro for you; there is no header
to edit and nothing to declare.
