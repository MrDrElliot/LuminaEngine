# External dependencies

Setup pulls one prebuilt bundle per platform from this repo's GitHub Releases (the
`external-deps` tag) and unpacks it into `External/`. They are release assets, not
committed files, so they stay out of git history and off LFS. `External/` is
git-ignored.

| Host | Asset | Status |
|---|---|---|
| Windows | `External-Win64.zip` (~192 MB) | published |
| Linux | `External-Linux64.tar.gz` (~186 MB) | published — rebuild it with [`BuildScripts/MakeLinuxBundle.sh`](BuildScripts/MakeLinuxBundle.sh) |

The bundles are designed to **coexist in one `External/` tree**, so a machine that
builds for both platforms installs both and neither overwrites the other. That works
without any platform subdirectory scheme because the payloads cannot collide on a
file name: the .NET runtime is already RID-scoped (`runtime/win-x64`,
`runtime/linux-x64`, matching `DotNetHost`'s `RuntimeRid()`), and everything else
differs by extension. Only the headers are shared, and those are identical.

Setup decides whether a bundle is installed by probing a single sentinel file for the
host platform — `LLVM/bin/libclang.dll` or `LLVM/lib/libclang.so` — rather than by
`External/` merely existing, so a checkout populated for the other platform still
fetches what this host needs.

The Linux bundle is a **tarball, not a zip**, and that is a requirement rather than a
preference. Slang's Linux libraries use versioned filenames with the plain name
symlinked onto them (`libslang.so` → `libslang-compiler.so.0.x.y`), and libclang is
packaged the same way; the SONAME recorded in the binaries is the versioned one, so
flattening those links into copies produces a loader error naming a library that is
visibly present. Zip also drops the executable bit, which `slangc`, the Tracy tools
and the .NET host all need. `ZipFile.ExtractToDirectory` restores neither, so the
Linux path goes through `TarFile` instead.

Everything in the bundle is open source and listed below. To skip the download,
fetch each library from its upstream and drop it into `External/` (see
[Building it yourself](#building-it-yourself)).

## Contents

| Library | Path (size) | Used for | License |
|---|---|---|---|
| .NET 10 runtime + hosting headers | `External/DotNet` (78 MB) | CoreCLR host for C# scripting (LuminaSharp) | MIT (headers); .NET redistributable terms (runtime) |
| LLVM / Clang 19 (libclang) | `External/LLVM` (337 MB) | Reflector parses C++ headers to generate reflection code | Apache-2.0 WITH LLVM-exception |
| Slang | `External/SLang` (155 MB) | compiles `.slang` shaders to SPIR-V | Apache-2.0 WITH LLVM-exception |
| RenderDoc | `External/RenderDoc` (24 MB) | in-app GPU frame capture | MIT |
| Tracy | `External/Tracy` (77 MB) | CPU/GPU profiler | BSD-3-Clause |

## Upstreams

- **.NET 10.0.2** (win-x64): <https://github.com/dotnet/runtime>, builds at <https://dotnet.microsoft.com/download/dotnet/10.0>. Headers are MIT (`src/native/corehost`); the runtime is Microsoft's redistributable.
- **LLVM / Clang 19.x** (commit `faef8b4`): <https://github.com/llvm/llvm-project>. Built from source because the official Windows installer ships `libclang.dll` without the import lib and headers needed to link against it.
- **Slang** (std module 2026.3.1): <https://github.com/shader-slang/slang>. `slang-llvm.dll` bundles LLVM, `slang-glslang.dll` wraps glslang, `gfx.dll` is Slang's deprecated graphics layer.
- **RenderDoc**: <https://github.com/baldurk/renderdoc>, builds at <https://renderdoc.org/builds>. Loaded dynamically, so a local install works too.
- **Tracy**: <https://github.com/wolfpld/tracy>.

## Verifying

Setup checks each bundle against the SHA-256 pinned in `SetupMode.Bundles`
(`Engine/Tools/LuminaBuildTool/Modes/SetupMode.cs`) before unpacking. The hash lives
in the repo rather than next to the download, so a tampered or swapped file is
rejected. Check it yourself:

```bash
sha256sum External-Linux64.tar.gz
```

```bat
powershell -NoProfile -Command "(Get-FileHash External-Win64.zip -Algorithm SHA256).Hash"
```

An empty pinned hash means no bundle has been published for that platform yet, and
Setup says so rather than downloading whatever the release serves for a missing
asset. After unpacking, Setup re-checks the sentinel file, so an asset that is the
wrong platform or incomplete fails at setup instead of during a build.

## Building it yourself

### Linux

[`BuildScripts/MakeLinuxBundle.sh`](BuildScripts/MakeLinuxBundle.sh) does this. Run it
on Linux (or WSL) — the payload carries symlinks and executable bits, and neither
survives being staged from Windows:

```bash
BuildScripts/MakeLinuxBundle.sh
```

It fetches each upstream, lays them out, **verifies the layout before packing**, and
prints the SHA-256 to pin. The verification is the part that matters: every path it
checks is one the build resolves by name, so a missing or renamed file is caught on
the machine that has the files rather than surfacing later as a link failure or a
shader compile that cannot start.

LLVM comes from the distribution's `libclang-$MAJOR-dev` package rather than an
llvm.org tarball. The asset naming on the LLVM releases has moved around
(`clang+llvm-<v>-x86_64-linux-gnu-ubuntu-<rel>` on older releases,
`LLVM-<v>-Linux-X64` more recently) and not all of them ship `libclang.so` alongside
the `clang-c` headers; `apt.llvm.org` carries every version on every release, which
is what makes the recipe reproducible instead of dependent on the host's default
clang.

### Runtime dependencies of the bundle

The bundle ships `libLLVM` and `libclang` but not the system libraries those link against, so a
machine missing them links the Reflector and then fails on a library the build never names:

```bash
sudo apt-get install -y libxml2 libzstd1 libffi8 libedit2 zlib1g
```

`Setup.sh` checks for these now. To see what a given copy actually needs:

```bash
ldd External/LLVM/lib/libLLVM.so.19.1 | grep "not found"
```

### By hand

Fetch each library from its upstream and lay it out to match the build's paths.
Required:

```
External/LLVM/include/clang-c/            Reflector's parse headers
External/LLVM/{bin,lib}/libclang.{dll,so} Reflector links it and stages it (non-optional)
External/SLang/lib/                       libslang.so / import libs
External/SLang/bin/                       slangc, and the .dll set on Windows
External/DotNet/include/                  hostfxr.h, nethost.h, coreclr_delegates.h
External/DotNet/runtime/<rid>/            RID-scoped: win-x64, linux-x64
```

Optional — their absence degrades rather than breaks:

```
External/RenderDoc/renderdoc.dll | librenderdoc.so    in-app GPU capture
External/Tracy/                                       profiler UI binaries
```

Then run `GenerateProjectFiles.bat`. Setup skips the download whenever this host's
sentinel file is already present.

## License compliance

When redistributing the bundle, ship each library's `LICENSE`/`NOTICE` next to its
binaries: Apache-2.0 for LLVM and Slang, BSD for Tracy, MIT for RenderDoc and the
.NET headers, plus .NET's `THIRD-PARTY-NOTICES.txt`. The .NET runtime binaries are
covered by Microsoft's redistribution terms, not MIT.

## What else setup does

- Persists `LUMINA_DIR` to `HKCU\Environment` so standalone game projects can find the engine. Remove with `reg delete "HKCU\Environment" /v LUMINA_DIR /f`.
- Points `core.hooksPath` at [`BuildScripts/Hooks`](BuildScripts/Hooks). The one hook, `post_merge`, wipes `Binaries/`, `Intermediates/`, and stale IDE files after a merge.
- Generates the IDE project files by calling `LuminaBuild.bat GenerateProjectFiles`.

## Updating a bundle

```bat
:: replace the asset (the URL stays the same)
gh release upload external-deps External-Win64.zip --clobber
gh release upload external-deps External-Linux64.tar.gz --clobber
```

Then repin: paste the hash into the matching entry's `Sha256` in `SetupMode.Bundles`,
in the same commit that uploads the asset. First time only, create the release with
`gh release create external-deps <asset> --title "External dependencies"`.

Both assets are named `External-<Platform>.<format>`. The formats differ because the
payloads do: the Linux one has to be a tarball to keep symlinks and executable bits,
which zip drops.

The release also still carries the old unsuffixed `External.zip`. Leave it there. Its
URL is baked into every checkout made before the rename, and deleting it would break
Setup for all of them; keeping it as a legacy alias costs nothing.
