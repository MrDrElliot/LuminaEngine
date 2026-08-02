# Plugins

Project plugins live here, one folder each. A plugin is a self-contained bundle of modules,
content and settings that can be turned on and off per project, which makes it the right home for
anything you might want to reuse in another project or ship separately from your game module.

This folder starts empty. It is not required, delete it if you never want one.

## Adding a plugin

From the editor: **Tools > Plugin Browser > New Plugin**. Give it a name, and the editor scaffolds
it here and regenerates your project files. Rebuild the solution and the new modules load on the
next editor launch.

By hand: copy `Templates/Plugin` from your engine install into this folder, rename it, and replace
the `$PLUGINNAME`, `$RUNTIMEMODULE` and `$EDITORMODULE` tokens in the file names and contents. Then
run `GenerateProject.bat` in the project root.

## What you get

A plugin named `Combat` is scaffolded as:

```
Plugins/Combat/
├── Combat.lplugin                  Descriptor: modules, loading phases, enabled-by-default
└── Source/
    ├── CombatRuntime/              Loaded in the editor and in a packaged game
    │   ├── CombatRuntime.Build.cs
    │   └── CombatRuntimeModule.h/.cpp
    └── CombatEditor/               Editor-only, stripped from packaged builds
        ├── CombatEditor.Build.cs
        └── CombatEditorModule.h/.cpp
```

Put gameplay code, reflected components and systems in the Runtime module. Put asset editors,
property customizations and menu entries in the Editor module.

Each module's `.Build.cs` is its whole build configuration: name its dependencies there, the way
`Source/$PROJECTNAME.Build.cs` does for your game module. Export a type from a module with that
module's `<MODULENAME>_API` macro; the build system defines it, so there is no header to edit.

## Naming

Plugin names must be unique across your project *and* the engine's own plugins. Discovery keys on
the name, so a collision means one of them is silently dropped. The editor rejects a duplicate name
up front.

## Enabling and disabling

A plugin's `.lplugin` decides whether it is on by default. Toggle it per project in the editor's
Plugin Browser, which records the choice in `$PROJECTNAME.lproject`.

A plugin that is off is not built and not loaded.

## In the IDE

Project plugins appear in the generated solution under `Games/$PROJECTNAME/Plugins/<PluginName>`,
alongside `Games/$PROJECTNAME/Source`. The engine's own plugins stay under a separate top-level
`Plugins` folder, so it is always clear which are yours.

Adding or removing a plugin changes the solution, so re-run `GenerateProject.bat` afterwards if you
did it by hand. The editor's New Plugin flow does this for you.
