Lumina Engine
=============

Run "Lumina Editor.exe" in this folder. Nothing else needs installing: there is no build step,
no Visual Studio and no .NET SDK to set up.

Keep the folder together
------------------------
The editor finds its fonts, shaders and plugins next to itself, so extract the whole zip and run
the launcher from the extracted folder. Put it somewhere you can write to, such as your Documents
folder or another drive. The editor keeps its shader cache and logs inside this folder, so it will
not run correctly from Program Files.

Requirements
------------
- Windows 10 or 11, 64-bit
- A GPU with an up to date Vulkan driver

Making a project
----------------
Choose Project > New Project in the editor. New projects hold content and C# scripts:
- Game\Content   assets you import or create in the editor
- Game\Scripts   C# scripts, compiled by the editor itself whenever you save a .cs file

Projects that contain C++ code, and packaging a finished game, need a source build of the engine.
Ask an engineer when you need either.

Version
-------
The version this package was built from is in Engine\InstalledBuild.json.
