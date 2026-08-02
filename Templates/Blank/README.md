# $PROJECTNAME

A Lumina Engine project.

## Requirements

- The `LUMINA_DIR` environment variable must point at your engine install (set by the engine's `Setup.bat`).
- Visual Studio 2026 (18.0+) with the C++ workload. (The engine's C# layer targets `net10.0`, which needs VS 18.0+; the standalone .NET 10 SDK alone is not enough.)

## First-time setup

1. Run `GenerateProject.bat` from this folder. It calls `%LUMINA_DIR%\LuminaBuild.bat GenerateProjectFiles` for this project and writes `$PROJECTNAME.sln`.
2. Open `$PROJECTNAME.sln` in **Visual Studio** or **JetBrains Rider**.
3. Press **F5**.

F5 builds the game DLL (`Binaries\Windows64\$PROJECTNAME-Development.dll`) and launches the Lumina
editor with this project pre-loaded. Breakpoints in your game module hit as soon as
`IMPLEMENT_MODULE` runs.

The solution also contains the engine's own targets, because the engine is built from source
alongside your project. Its output stays in the engine tree and is shared by every project, so it
is built once and not again.

- **Visual Studio** uses the generated `.vcxproj.user`, which points Run at the editor with `--Project=` set to this project.
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
- **C++** (`Source/*.cpp` / `*.h`): press F5 again to rebuild the DLL and relaunch the editor. New `.h` / `.cpp` files are picked up by the build automatically; re-run `GenerateProject.bat` to make them show up in the IDE's file list.

## Project layout

```
$PROJECTNAME.lproject          Project descriptor (name, GUID, plugins)
GenerateProject.bat            Regenerate the .sln after adding or removing source files
Config/GameSettings.json       Per-project engine settings (startup maps, cook roots, ...)
Source/                        Your C++ module (optional)
  $PROJECTNAME.Target.cs       What to build: names the launch module, points Run at the editor
  $PROJECTNAME.Build.cs        How to build it: dependencies, defines, include paths
Plugins/                       Project plugins, one folder each (see Plugins/README.md)
Game/Content/                  Assets, surfaced to the engine under /Game/Content
Game/Scripts/                  C# scripts, compiled in-editor (surfaced under /Game/Scripts)
```

`Binaries/` and `Intermediates/` are build output and are ignored by git.

## Adding to the C++ module

`Source/$PROJECTNAME.Build.cs` is the whole build configuration for the module. To use another
engine or third-party module, name it there:

```csharp
PrivateDependencyModuleNames.Add("JoltPhysics");
```

Export a type from the module with the `$PROJECTNAMEUPPER_API` macro, as
`Source/$PROJECTNAMEModule.h` does. The build system defines that macro for you; there is no header
to edit and nothing to declare.
