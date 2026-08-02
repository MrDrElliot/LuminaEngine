# $PLUGINNAME

$PLUGINDESCRIPTION

A Lumina plugin scaffolded from the editor. It ships two modules:

- **$RUNTIMEMODULE** (`Runtime`), gameplay code loaded in both the editor and
  packaged game. Add reflected components/systems, Lua bindings, etc. here.
- **$EDITORMODULE** (`Editor`), editor-only customizations, stripped from
  packaged builds. Register asset editor tools, property customizations, and
  menus here.

## Layout

```
$PLUGINNAME/
├── $PLUGINNAME.lplugin            Descriptor (modules, loading phases)
└── Source/
    ├── $RUNTIMEMODULE/            Sources plus $RUNTIMEMODULE.Build.cs
    └── $EDITORMODULE/             Sources plus $EDITORMODULE.Build.cs
```

Each module directory holds its own `.Build.cs` alongside its sources; that file
is the module's whole build configuration.

## Building

The plugin is discovered by the owning project when it generates its IDE files.
After creating it, run the project's `GenerateProject.bat` (or use the editor's
New Plugin flow, which does it for you), then rebuild. The new module DLLs load
on the next editor launch.

Export a type from a module with the module's `<MODULENAME>_API` macro, as the
generated module headers do. The build system defines it; there is no header to
edit and nothing to declare.

## Enabling / disabling

Toggle the plugin per-project in the editor's **Plugin Browser**, which writes
an entry into the project's `.lproject`.
