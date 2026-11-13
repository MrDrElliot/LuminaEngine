Lumina Engine
=============

Run "Lumina Editor.exe" (Windows) or "Lumina Editor" (Linux) in this folder. Nothing else needs
installing: there is no build step, no Visual Studio and no .NET SDK to set up.

Keep the folder together
------------------------
The editor finds its fonts, shaders and plugins next to itself, so extract the whole archive and run
the launcher from the extracted folder. Put it somewhere you can write to, such as your Documents
folder or another drive. The editor keeps its shader cache and logs inside this folder, so it will
not run correctly from Program Files or another read-only location.

On Linux, extract the .tar.gz with tar so the binaries keep their executable bit. The engine's
shared libraries sit beside the editor in Binaries/Linux64, where the editor looks for them.

Requirements
------------
- Windows 10 or 11, 64-bit, or a 64-bit Linux distribution with glibc and libstdc++
- A GPU with an up to date Vulkan driver

Making a project
----------------
Choose Project > New Project in the editor. New projects hold content and C# scripts:
- Game\Content   assets you import or create in the editor
- Game\Scripts   C# scripts, compiled by the editor itself whenever you save a .cs file

Projects that contain C++ code, and packaging a finished game, need a source build of the engine.
Ask an engineer when you need either.

How the Linux package is built
------------------------------
LuminaBuild Package -Platform=Linux64 builds with the engine's clang or gcc toolchain on a Linux host.
On a Windows host it builds inside WSL rather than cross-compiling: the tree is mirrored to
~/.lumina/wsl in the default distribution, the Linux dependency bundle is fetched there on first use,
and every Binaries/Linux64 folder is copied back. The distribution needs the .NET 10 SDK, a compiler
the tree accepts (set CXX in ~/.profile) and the X11 development packages Setup.sh lists. Packaging a
project for Linux64 from the editor takes the same path. Binaries copied out of a Windows folder lose
their executable bit, so chmod +x the game after moving it onto a Linux filesystem.

Version
-------
The version this package was built from is in Engine\InstalledBuild.json.
