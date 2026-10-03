# $PROJECTNAME

A Lumina Engine project.

## Opening it

Run `Lumina Editor.exe` from the engine folder and pick this project from the project list, or use
**Browse for project file** and choose `$PROJECTNAME.lproject`.

## Scripting (C#)

Gameplay is written in **C#** with `LuminaSharp`. Scripts live in `Game/Scripts/` and are **compiled inside the editor**: edit a `.cs`, save, and the change is live. There is no build step and nothing to restart.

- A script is a class deriving from `EntityScript` (see `Game/Scripts/ExampleScript.cs`). It gets `Entity`, `World`, `Registry`, a cached `Transform`, and lifecycle hooks (`OnAttach` / `OnReady` / `OnUpdate` / `OnDetach`) plus input and collision callbacks.
- Attach a script to an entity by adding a **C# Script** component and selecting the script class. Fields marked `[Property]` show up in the inspector.
- `Game/Scripts/<...>.Scripts.csproj` is **generated** for IDE IntelliSense only. It is recreated whenever the project loads, so never commit it.

## Iterating

- **C# scripts** (`Game/Scripts/*.cs`): save in your editor; the running engine recompiles and reloads them.
- **Content** (assets in `Game/Content/`): hot-reloads inside the editor.

## Project layout

```
$PROJECTNAME.lproject          Project descriptor (name, GUID, plugins)
Config/GameSettings.json       Per-project engine settings (startup maps, cook roots, ...)
Plugins/                       Project plugins, one folder each (see Plugins/README.md)
Game/Content/                  Assets, surfaced to the engine under /Game/Content
Game/Scripts/                  C# scripts, compiled in-editor (surfaced under /Game/Scripts)
Logs/                          Engine log for runs with this project loaded (Lumina.log, 5 kept)
CrashDumps/                    Minidumps and GPU crash dumps from runs with this project loaded
```

`Binaries/`, `Intermediates/`, `Logs/` and `CrashDumps/` are generated output and are ignored by git.

## C++

This project was made with a prebuilt engine, so it has no C++ module. Adding native code needs a
source build of the engine; an engineer can add a `Source/` module to the project from one.
