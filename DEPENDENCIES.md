# External dependencies

`Setup.bat` pulls one prebuilt bundle, `External.zip` (~671 MB), from this repo's
GitHub Releases (the `external-deps` tag) and unzips it into `External/`. It is a
release asset, not a committed file, so it stays out of git history and off LFS.
`External/` is git-ignored.

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

Setup checks `External.zip` against the SHA-256 in `EXPECTED_SHA256`
([Setup.lua](BuildScripts/Actions/Setup.lua)) before extracting. The hash lives in
the repo rather than next to the download, so a tampered or swapped file is
rejected. Check it yourself:

```bat
powershell -NoProfile -Command "(Get-FileHash External.zip -Algorithm SHA256).Hash"
```

## Building it yourself

Fetch each library from its upstream and lay it out to match the build's paths:

```
External/DotNet/{bin,include,runtime}    runtime + hosting headers
External/LLVM/{bin,lib,include}          libclang.dll, import libs, headers
External/SLang/{bin,lib} + *.dll         compiler DLLs
External/RenderDoc/renderdoc.dll
External/Tracy/*.exe
```

Then run `GenerateProjectFiles.bat`. Setup skips the download whenever `External/`
already exists.

## License compliance

When redistributing the bundle, ship each library's `LICENSE`/`NOTICE` next to its
binaries: Apache-2.0 for LLVM and Slang, BSD for Tracy, MIT for RenderDoc and the
.NET headers, plus .NET's `THIRD-PARTY-NOTICES.txt`. The .NET runtime binaries are
covered by Microsoft's redistribution terms, not MIT.

## What else setup does

- Persists `LUMINA_DIR` to `HKCU\Environment` so standalone game projects can find the engine. Remove with `reg delete "HKCU\Environment" /v LUMINA_DIR /f`.
- Points `core.hooksPath` at [`BuildScripts/Hooks`](BuildScripts/Hooks). The one hook, `post_merge`, wipes `Binaries/`, `Intermediates/`, and stale IDE files after a merge.
- Generates the IDE project files by calling `LuminaBuild.bat GenerateProjectFiles`.

## Updating the bundle

```bat
:: replace the asset (the URL stays the same)
gh release upload external-deps External.zip --clobber
```

Then repin the hash: run `Get-FileHash` (above) and paste the value into
`SetupMode.ExpectedSha256` in the same commit. First time only, create the release with
`gh release create external-deps External.zip --title "External dependencies"`.
