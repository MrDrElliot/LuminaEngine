# Linux / GCC / Clang support

Assessment of what stands between the current tree (Windows + MSVC only) and a Linux x86-64 build
with GCC or Clang. Ordered by what blocks what: nothing in Tier 2 can be tested until Tier 0 and
Tier 1 exist.

Status legend: `[ ]` not started, `[~]` partial, `[x]` done.

---

## Where this stands (2026-08-13)

**The Linux editor builds clean and runs, from the published bundle alone.** A clean build is
253/253 actions with zero errors, producing `Binaries/Linux64/Lumina-Development` (ELF PIE, every
shared library resolved). Verified by wiping `External/`, extracting only `External-Linux64.tar.gz`
into it, and rebuilding from scratch -- so the bundle is self-sufficient, not propped up by anything
that happened to be installed on the build host.

Verified running under WSL/WSLg: logging, the crash handler (it caught a SIGABRT and wrote a
symbolised stack trace plus a crash report), engine-root resolution, GLFW window creation, plugin
discovery and `dlopen` of a plugin module (`Networking` loads and initialises its ENet backend), the
job system (30 workers, 256 fibers on the hand-written x86-64 SysV context switch) and Jolt routed
through it.

It stops at Vulkan on that box because the WSL instance has no Vulkan ICD at all. **The renderer and
C# scripting are the two parts still unexercised** -- both need a machine with a real GPU driver, and
scripting initialises after the renderer.

Windows-only by design: the BugSplat crash reporter and the NsightPerf plugin, both gated.
`zenity` is optional; without it, dialogs degrade to log messages.

### The bundle

`BuildScripts/MakeLinuxBundle.sh` assembles `External-Linux64.tar.gz` (186 MB) and verifies its own
layout before packing. Versions come from `Engine/Build/DependencyVersions.txt`, which
`SLang.Build.cs` reads too -- they were written down separately and had already drifted (the script
fetched Slang 2026.14.1 while the build expected 2026.3.1).

Two traps this class of bundle keeps hitting, both now handled and both invisible on a developer box:
- **Symlinks that point out of their own tree.** `lib/libclang.so` and, less obviously,
  `include/llvm` and `include/llvm-c` are all symlinks into the distro's multiarch directories.
  `cp -a` stages them as dangling links.
- **Staging a name that is not the SONAME.** The loader asks for `libclang-19.so.19`, so staging
  `libclang.so` alone resolves against a system install if one exists and fails if it does not.

---

## Tier 0 — LuminaBuildTool

The tool itself is already structured for this. `IBuildPlatform` / `IToolchain` are clean seams,
`BuildPlatform.Linux64` exists, `BuildPlatformRegistry.HostPlatform` already detects Linux, and
`GetSystemName` / `GetOutputDirectoryName` / `GetSharedLibraryExtension` already answer for it.
What is missing is an implementation behind those seams.

- [x] **`LinuxPlatform : IBuildPlatform`** — `Platform/Linux/LinuxPlatform.cs`. `lib` prefix,
      `.so` / `.a` / `.o`, no import library, `DLL_EXPORT` as
      `__attribute__((visibility("default")))` and `DLL_IMPORT` empty, plus `LE_PLATFORM_LINUX`,
      `PLATFORM_UNIX=1` and `_GNU_SOURCE`. Registered in `BuildPlatformRegistry`.
- [x] **`ClangToolchain : IToolchain`** — `Toolchain/Linux/ClangToolchain.cs`, driving clang++ or
      g++ from one class. Everything below is implemented; what it has not yet survived is a real
      Linux host, since the only verification available so far was a cross-invocation from Windows
      that reaches `'cstddef' file not found` for want of a sysroot.
      - `/Fo /I /D /FI /wd /we /WX /W# /std: /EH /GR /TP /TC /arch: /Od /O2 /Ob /Gy /GL /Z7 /MD /MT`
      - `/OUT /DLL /IMPLIB /SUBSYSTEM /DEBUG /PDB /OPT:REF /OPT:ICF /INCREMENTAL /LIBPATH /LTCG /MACHINE`
      - `/WHOLEARCHIVE:x` → `-Wl,--whole-archive x -Wl,--no-whole-archive` (must *bracket*, and the
        monolithic module-registration trick depends on getting this right)
      - **`-fPIC`** on every object that can land in a `.so`. Free on Windows, mandatory here, and
        the failure mode is a link error deep in a static library.
      - **`-fvisibility=hidden -fvisibility-inlines-hidden`** to approximate dllexport semantics.
        Without it ELF exports everything and interposition changes behaviour (see the allocator
        item in Tier 3).
      - **`-Wl,-rpath,$ORIGIN`** — there is no "search the executable's directory first" rule on
        Linux. Every module `.so`, plus Slang and libclang, depends on this.
      - `--start-group` / `--end-group` for the static-library cycles MSVC resolves by rescanning.
      - `--target=x86_64-unknown-linux-gnu` pinned on Clang. Without it a Clang built for another
        host silently produces objects for that host and writes them into `Binaries/Linux64`.
        Not passed to GCC, which is built for one target and rejects it.
      - `ar` has no truncating mode, so the archive action clears its output first (new
        `BuildAction.bDeleteOutputsBeforeRun`). Without that a source that stops being compiled
        leaves its object in the library forever.
- [x] **Header-dependency format.** `BuildAction.DependencyListFormat` selects the parser;
      `DependencyCache.ParseMakefileDependencies` reads the `-MD -MF` fragment, handling line
      continuations, escaped spaces and the phony rules `-MP` would add.
- [x] **PCH.** Clang precompiles the header to an explicit path and consumes it with
      `-include-pch`. GCC can only find `<header>.gch` beside the header itself, which would mean
      writing generated files into the source tree, so it force-includes the header uncompiled
      instead — slower, never subtly stale. Worth revisiting if GCC build times become a problem.
- [x] **Toolchain locator.** `Toolchain/Linux/LinuxToolchainLocator.cs`. Honours `CXX`/`CC`/`AR`,
      falls back to PATH preferring Clang, picks `llvm-ar`/`gcc-ar` over `ar` so LTO archives get an
      index, prefers lld/mold/gold over bfd, and derives `VersionKey` from the compiler banner.
- [x] **Response-file quoting.** Added `PathUtils.QuoteUnix`. The existing `Quote` implements
      `CommandLineToArgvW`, which passes Windows path separators through untouched; the GNU-style
      parser treats every backslash as an escape and ate them, so paths arrived as
      `H:LuminaEngineEngineSource...`. Confirmed by the first cross-invocation.
- [ ] **`DependencyCache` / `FileItem` use `OrdinalIgnoreCase`** for path keys. Two files differing
      only in case are distinct on Linux and would be conflated.
- [ ] **Project generation.** `VisualStudioGenerator` is the only generator. Linux needs
      `compile_commands.json` at minimum (`CompileDatabaseStep` exists — verify it is
      toolchain-neutral once a second toolchain exists).
- [x] **Entry scripts.** `LuminaBuild.sh` and `Setup.sh`, verified running the tool on Linux.
      `Setup.sh` checks the prerequisites that actually bite — a GCC older than 13 (no `<format>`)
      and the X11 `-dev` packages GLFW links — because both otherwise surface much later as a
      compiler or linker error with no obvious cause.
      `GenerateProjectFiles.bat` has no counterpart yet; it only drives the Visual Studio generator,
      so it is not on the path to a Linux build.
- [ ] **`HostCapabilities`** shells out to `powershell`; already guarded by an OS check, but the
      Linux path returns nothing useful.

### Rules files carrying MSVC flags unconditionally

- [x] `Engine/Build/Lumina.BuildRules.cs` — `/bigobj`, `/NODEFAULTLIB:LIBCMT` and the MSVC warning
      numbers are now behind a `Windows64` check. `CppStandard` and `VectorExtensions` stay as they
      are and the toolchain translates them (`c++latest` → `-std=c++23`, `AVX` → `-mavx`), since
      both name a build setting rather than a flag.
- [x] `Engine/Applications/Reflector/Reflector.Build.cs` — `/NODEFAULTLIB:MSVCRTD` and the LLVM
      component libraries are Windows-only; Linux links the single `clang` shared library and stages
      `libclang.so`.
- [ ] `bUseDynamicCrt` / `bUseDebugCrt` have no Linux meaning and are currently ignored by the
      Clang toolchain; the equivalent knob is libstdc++ vs libc++, which does not exist yet.
- [ ] `GlobalDisabledWarnings` remains an MSVC-number list by convention. The Clang toolchain drops
      purely numeric entries and passes names through as `-Wno-<name>`, so both spellings can
      coexist, but the first real Linux build will want a named set alongside it.

---

## Tier 1 — Prebuilt dependencies

`SetupMode` is now per-platform: `SetupMode.Bundles` holds one entry per host, selected by
`BuildPlatformRegistry.HostPlatform`. Installation is decided by probing a sentinel file
(`LLVM/lib/libclang.so`) rather than by `External/` merely existing, so a checkout populated for
Windows still fetches what a Linux host needs. An unpinned hash means "not published yet" and is
reported as such instead of downloading whatever the release serves for a missing asset, and the
sentinel is re-checked after unpacking so a wrong or incomplete asset fails at setup.

**The bundles coexist in one `External/` tree — no restructure, and no republish of the Windows
asset.** The payloads cannot collide on a file name: .NET is already RID-scoped
(`runtime/win-x64`, `runtime/linux-x64`, matching `DotNetHost`'s `RuntimeRid()`, which already
returns `linux-x64`), and everything else differs by extension. Only the headers are shared, and
those are identical.

**The Linux bundle is a tarball, not a zip, and that is a requirement.** Slang's Linux libraries
use versioned filenames with the plain name symlinked onto them
(`libslang.so` → `libslang-compiler.so.0.x.y`), and libclang is packaged the same way; the SONAME
recorded in the binaries is the versioned one, so flattening those links into copies produces a
loader error naming a library that is visibly present. Zip also drops the executable bit, which
`slangc`, the Tracy tools and the .NET host all need. `ZipFile.ExtractToDirectory` restores
neither, so the Linux path goes through `TarFile`; a round-trip probe confirmed it carries
`SymbolicLink` entries with their `LinkName` and the `UserExecute` mode bit.

- [x] **Per-platform setup, sentinel probing, and tar extraction.**
- [x] **`BuildScripts/MakeLinuxBundle.sh`** — fetches each upstream, lays it out, verifies the
      layout before packing, and prints the SHA-256 to pin. The verification is the valuable part:
      every path it checks is one the build resolves by name, so a rename is caught on the machine
      that has the files. Both its pass and fail paths were exercised against synthetic trees.
- [ ] **Actually build and publish the bundle.** Needs a Linux host; this is the remaining blocker
      for compiling a single Linux translation unit. Everything below is what it must contain.

- [x] **.NET 10 runtime + hosting headers** → the build side needed nothing: `runtime/<rid>` and
      `bin/<rid>` are already RID-scoped and `RuntimeRid()` already answers `linux-x64`.
      `DotNetHost.cpp`'s `kSharedExt` already covers `.so`. The bundle script installs it via
      `dotnet-install.sh --runtime dotnet`.
- [x] **LLVM/Clang 19 libclang** → `Reflector.Build.cs` stages `libclang.so` and links the single
      `clang` shared library on Linux, instead of the MSVC-style component list.
- [x] **Slang** → `SLang.Build.cs` now stages `lib/libslang*.so` on Linux instead of `bin/*.dll`.
      Note Slang puts its shared libraries in `lib/` on Unix and `bin/` on Windows.
- [ ] **RenderDoc** → bundle script stages `librenderdoc.so`, but `RenderDocImpl.cpp` still calls
      `GetModuleHandleW(L"renderdoc.dll")`. Tier 2 work; capture is simply unavailable until then.
- [x] **Tracy** — source only, fine as-is.

In-tree Windows-only vendor binaries, each needing a platform guard in its `.Build.cs`:

- [ ] `BugSplat` — Windows-only product; gate the whole module off on Linux.
- [ ] `NvidiaAftermath` — Linux `.so` exists upstream; currently `.lib` + `.x64.dll`.
- [ ] `NsightPerf` — `nvperf_grfx_host.dll`; Linux host library exists upstream.

Third-party source modules that build fine on Linux but whose `.Build.cs` does not gate sources or
system libraries per platform yet. Only `GLFW` and `ENet` currently do.

- [ ] Audit all 34 `Source/ThirdParty/*/**.Build.cs` for unguarded Windows assumptions.

---

## Tier 2 — Runtime platform layer

There is no `Source/Runtime/Source/Platform/Linux/` at all. ~2700 lines of Windows implementation
have no counterpart.

Two things are already better than they look. **Every Windows-only source is guarded** with
`#ifdef LE_PLATFORM_WINDOWS` (or `_WIN32`) at the top — `WindowsPlatformProcess.cpp`,
`WindowsCrashHandler.cpp`, `WindowsCrashReporter.cpp`, `WindowsDirectoryWatcher.cpp`,
`WindowsThread.cpp`, `WindowsDialogs.cpp`, `Fiber.cpp`, `DLLMain.cpp` — so the scanner can keep
globbing them and they compile to empty translation units. No per-platform exclusion rules needed.
And `WindowsPlatformString.cpp` **already carries a complete portable fallback** behind its `#else`,
hand-rolling UTF-8 ↔ UTF-16/UTF-32 and branching on `sizeof(WIDECHAR) >= 4`, so the string
conversion entry points are done.

- [x] **`PlatformProcess.h`** — `Platform/Linux/LinuxPlatformProcess.cpp`. All 29 declared entry
      points implemented, verified by diffing the header's declarations against `nm` on the
      compiled object rather than by eye. `GenericPlatformProcessStubs.cpp` is now guarded off for
      Linux and left as the fallback for a platform nobody has ported yet.

      **Verified by running it on a real Linux host** (WSL Ubuntu, GCC 13), not just compiling:
      a harness links the real object against stubs for the ~14 engine symbols it needs, and
      exercises 34 behaviours — process identity, monotonic time, CPU topology, `/proc` memory and
      smaps parsing, `mallinfo2` heap figures, env round-trip, `dlopen`/`dlsym` with an actually
      callable resolved symbol, exit-code propagation, merged stdout+stderr capture, working
      directory, argument tokenizing, unterminated final lines, 20k lines of output without
      deadlocking, and detached launch. All pass.

      Two bugs the harness caught that review had not:
      - `dlerror()` was called twice in one expression — the first call *clears* the error, so the
        second returned null and the `FString` constructed from it segfaulted.
      - The argument tokenizer applied shell backslash rules. These callers build Windows command
        lines, where a backslash is only special directly ahead of a quote, so a parameter
        containing `\n` was arriving as a bare `n`. It now follows `CommandLineToArgvW`, including
        the backslash-run rule, and single quotes are left for the receiving program.

      Notable design points: `posix_spawn` for the waiting forms (safe in a threaded process),
      double-fork + `setsid` for detached (so init reaps it rather than leaving a zombie, and the
      child does only async-signal-safe work before `execv`); `PushDLLDirectory` cannot do what
      `SetDllDirectory` does, since the loader reads `LD_LIBRARY_PATH` once at startup — the list
      only helps resolve libraries we name ourselves, which is why every binary is linked with
      `-rpath $ORIGIN`.
- [x] **Fibers** — `Core/Threading/LinuxFiber.cpp`. Tested on Linux at both `-O0` and `-O2`.

      A hand-written x86-64 SysV context switch, not `ucontext`: `swapcontext` calls `sigprocmask`
      on every switch, and a fiber scheduler switches at every job boundary and every wait, so that
      syscall lands squarely on the hot path for a signal mask nothing here wants saved. The stub
      saves rbx/rbp/r12–r15 plus MXCSR and the x87 control word — the FP pair matching the Windows
      backend's `FIBER_FLAG_FLOAT_SWITCH`, without which a fiber that changes rounding mode or
      unmasks an exception leaks it into whatever runs next on that thread (Jolt does exactly this
      under `JPH_FLOATING_POINT_EXCEPTIONS_ENABLED`).

      Written as file-scope `asm()` inside the `.cpp` rather than a separate `.S`, so no new file
      type has to reach the build system. Stacks are `mmap`ed with a `PROT_NONE` guard page below
      them, which turns an overflow into a fault at the boundary instead of silent corruption of
      the neighbouring mapping.

      Verified: 100k round trips; callee-saved registers intact across a switch; FP rounding mode
      changed inside a fiber and confirmed not to leak back; 300 nested ~1 KB frames; **the same
      fiber resumed 3200 times from 16 different OS threads with its stack state verified intact
      each time**; and a deliberate overflow faulting on the guard page rather than running past it.

      One bug only `-O2` caught: ending the asm block with a bare
      `.section .note.GNU-stack` leaves the assembler *in* that section, so everything the compiler
      emitted afterwards landed there and the link failed with "defined in discarded section". It
      needs `.pushsection`/`.popsection`. An `-O0` build hides it, because the optimizer changes
      what gets emitted after the block.
- [x] **Threads** — `Core/Threading/LinuxThread.cpp`. Verified on a real Linux host, 24 checks.

      The interesting part was not in the Windows-only file but in the shared `Thread.cpp`:
      `GetThreadID()` read **`std::thread::id::_Get_underlying_id()`**, an MSVC-private member that
      libstdc++ does not have at all. It is now per-platform and asks the OS —
      `GetCurrentThreadId()` on Windows (the same number, since MSVC's `thread::id` *is* the Win32
      id) and `gettid()` on Linux, which is the id `top` and `perf` report and therefore actually
      worth having in a log record. Cached in a `thread_local` because it is read once per log
      record and `gettid` is a real syscall, where the Windows call is a TEB read.

      `pthread_setname_np` **rejects** an over-long name rather than truncating it, so a name past
      15 characters would have left the thread unnamed; the implementation truncates first. Tracy
      still receives the full name, so a truncated kernel name never costs the readable one in a
      profile. `SetThreadPerformanceHint` reports false — the nearest scheduler knob is uclamp's
      minimum-utilisation hint, which needs `CAP_SYS_NICE`.
- [x] **`PlatformString.cpp`** — the `#else` branch of `WindowsPlatformString.cpp` already
      implements all four conversion entry points portably, for 2- and 4-byte `wchar_t` alike.
      Worth moving out of `Platform/Windows/` at some point, since the name now undersells it.
- [x] **Directory watching** — `Platform/Filesystem/LinuxDirectoryWatcher.cpp`, tested on Linux,
      21 checks. All inotify state is local to the watch thread, so the header is shared with the
      Windows back end unchanged.

      The three ways inotify differs from `ReadDirectoryChangesW`, all of which had to be handled:
      - **No recursion.** Every subdirectory needs its own descriptor, including ones created after
        the watch started — those get one added as they appear.
      - **A directory arriving says nothing about its contents.** A move is one atomic event, so a
        populated folder pasted or moved into the tree would show up empty. Its contents are walked
        and reported explicitly. This can report a file twice (once here, once from the kernel),
        which is the right way round to be wrong: a duplicate Added looks like a second save, a
        missing one leaves the caller permanently out of step with the disk.
      - **Renames arrive as two events** (`IN_MOVED_FROM`/`IN_MOVED_TO`) paired by a cookie. They
        are joined into the single `Renamed` event the struct documents and `ContentBrowser`'s
        `TextAssetRenamed(Old, New)` expects. An unpaired `FROM` means the file left the tree and
        is reported as `Removed`.

      `Modified` maps to `IN_CLOSE_WRITE` rather than `IN_MODIFY`: "a writable handle was closed",
      i.e. saved, instead of one event per write. `IN_Q_OVERFLOW` is logged, since nothing can
      recover the dropped events and a caller working from a silently stale view is worse.

      **Windows discrepancy worth knowing about, not changed here.** The consumer expects one
      `Renamed` carrying old and new. The Windows back end emits *two*: the first sets
      `Path` and `OldPath` to the same old path, the second sets only the new path — so
      `TextAssetRenamed` gets called `(old, old)` and then `("", new)`. The Linux implementation
      follows the documented contract; Windows looks unintended and is left alone deliberately.
- [ ] **Crash handling** (`WindowsCrashHandler.cpp` 620 + `WindowsCrashReporter.cpp` 400) — SEH,
      `MiniDumpWriteDump`, dbghelp symbol resolution → `sigaction` on SIGSEGV/SIGBUS/SIGFPE/SIGABRT
      with an alternate signal stack, plus libunwind or `backtrace_symbols`.
- [x] **File dialogs** — `OpenFileDialogue`/`OpenFileDialogueMulti` are in
      `LinuxPlatformProcess.cpp` (zenity), and the message-box API is
      `Tools/Dialogs/LinuxDialogs.cpp`. `Dialogs::ShowInternal` had no non-Windows body at all, so
      it was an undefined symbol rather than a compile error. zenity reports the pressed button
      through its exit code only, so the multi-button types collapse onto affirmative/negative;
      with zenity absent the message goes to the log and a *question* answers Cancel, never Yes —
      a prompt nobody can see must not read as consent.
- [x] **`RenderDocImpl.cpp`** — had an unguarded `#include <windows.h>` and
      `GetModuleHandleW(L"renderdoc.dll")`. The library name is now per-platform
      (`librenderdoc.so`) and the "already loaded" check goes through `dlopen(RTLD_NOLOAD)`,
      deliberately not `Platform::GetDLLHandle`, which would *load* it — turning "are we being
      captured" into "start capturing".
- [x] **`Editor/Source/UI/EditorUI.cpp`** — its unguarded `#include <Windows.h>` turned out to be
      entirely dead; removed, and the Windows Editor module still builds and links.
- [x] **GLFW X11 libraries.** The Linux branch of `GLFW.Build.cs` set `_GLFW_X11` and listed the
      sources but declared no `PublicSystemLibraries`, so it would have compiled every GLFW source
      and then failed at link. Now links X11, Xrandr, Xinerama, Xcursor, Xi and xkbcommon, and
      picks up `posix_poll.c`, which was missing from the source list.
      Build host needs: `libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev
      libxkbcommon-dev`.
- [x] **Module export macros.** `IMPLEMENT_MODULE` and `LUMINA_MODULE_IMGUI` now emit `DLL_EXPORT`
      rather than `__declspec(dllexport)`. Verified end to end on Linux: a module built the way the
      engine builds one (`-fvisibility=hidden`) is `dlopen`ed and all four entry points resolve by
      name, a virtual call dispatches across the image boundary, and an undecorated control symbol
      stays hidden — so the exports are surviving hiding rather than hiding being off.

      **The ABI signature was silently broken on any non-MSVC compiler.** It embedded
      `LUMINA_MODULE_ABI_STR(_MSC_VER)`, which on GCC and Clang stringifies an undefined macro to
      the literal text `_MSC_VER` — the same token for every non-MSVC compiler and version, which
      is precisely the case the guard exists to catch. It now names the compiler family and major
      version, plus the standard library, because Clang builds against either libstdc++ or libc++
      and `_GLIBCXX_USE_CXX11_ABI` selects between two incompatible `std::string` layouts within
      libstdc++ alone. Windows reads `LMABI/2|Development|Editor|MSC1944|MSSTL`, Linux
      `LMABI/2|Debug|Editor|GCC13|GLIBCXX1`.

      Minor versions are deliberately out of the GCC/Clang token: both keep the Itanium C++ ABI
      stable across a major version, so including the minor would reject compatible plugins. MSVC
      keeps its full `_MSC_VER`, which has no such guarantee.

      **Note:** the Windows signature changed too (it gained `|MSSTL`), so any prebuilt plugin
      built against the old string is now correctly rejected until rebuilt.
- [x] **`DLLMain.cpp`** — `__attribute__((constructor))`/`((destructor))` for the process-attach and
      detach cases. There is deliberately nothing for thread attach/detach: ELF has no such
      notification and attach does not need one, because `FMalloc`'s primitives call
      `rpmalloc_thread_initialize` on every allocation and it is idempotent — the same reason
      monolithic Shipping works on Windows without a main-exe `DllMain`. Detach is a bounded
      retention rather than a leak (an exiting foreign thread's heap is reused when its id is), and
      closing it properly belongs with the allocator-interposition question below.
- [x] **`Memory.cpp`** — the four `/EXPORT` pragmas are now Windows-guarded, and the declarations
      carry a visibility attribute on ELF instead. The header documents why Windows cannot simply
      decorate them: a vendored TU declaring them plain while the header says `dllimport` is a
      linkage clash there. ELF has no such conflict, because visibility is not linkage.
- [x] **`HangWatchdog.cpp`** — not a blocker after all. The whole file is already
      `LE_PLATFORM_WINDOWS`-guarded *and* carries a complete no-op stub block for every other
      platform, so it links. The feature is simply absent on Linux: stall *detection* is portable
      (atomics and `steady_clock`), only the capture is not (`RtlVirtualUnwind`, `SuspendThread`,
      dbghelp). Worth splitting later so detection and the registered reporters still fire.
- [x] **`MemoryTracking.cpp`** — its `<Windows.h>` is already `LE_PLATFORM_WINDOWS`-guarded.
      Whether the tracking it provides degrades gracefully on Linux is untested.
- [x] **`GUID.cpp`** — verified on Linux, 19 checks. It already *had* a Linux branch, but it called
      `uuid_generate` from `<uuid/uuid.h>`: libuuid is a separate package (`uuid-dev`) and a
      link-time dependency (`-luuid`) that no build rule declares, so it would not have linked. It
      now reads the kernel CSPRNG directly through `getrandom` with a `/dev/urandom` fallback —
      which is what `uuid_generate` does internally anyway — and stamps the RFC 4122 version 4 and
      variant bits that `CoCreateGuid` and `CFUUIDCreate` produce for free. `ldd` on the test binary
      confirms no libuuid. A failed read returns an empty GUID rather than falling through to the
      PRNG: a caller checking `IsValid` can react, where a predictable GUID collides silently later.
- [x] **`Log/Sinks/StdoutSink.cpp`** — its `#else` branch already called `isatty`/`fileno` but
      `<unistd.h>` was never included, so the file had only ever compiled on Windows. One include.
- [ ] **`Scripting/DotNet/DotNetHost.cpp`** — 12 `_WIN32` sites.
- [x] **Entry point** — `Applications/Lumina/Source/Platform/LaunchLinux.cpp`, and
      `LaunchWindows.cpp` is now guarded (it was not). Both exist only to reach `LuminaMain`, which
      is where everything real happens, so the launcher is thin by design.

      It does one thing beyond forwarding: **ignore `SIGPIPE`.** Its default action is termination,
      with no chance to log and no crash dump — the process simply vanishes. ENet already passes
      `MSG_NOSIGNAL` on every send, so the engine's own traffic is covered, but that only covers
      code we can see; a vendored library, the C# runtime, or a profiler connection writing to a
      peer that just went away would take the process with it. It matters most where it is hardest
      to notice: a dedicated server (`-server`) spends its life writing to clients that disconnect
      without warning, and "the server occasionally exits with no log line" is the shape this takes.
      Ignoring it makes the write return `EPIPE` instead, which callers already handle because that
      is what Windows gives them.

      Verified standalone (the launcher has no engine dependencies): argument passthrough including
      an argument containing a space, and a write to a closed pipe returning `EPIPE` rather than
      killing the process.

      `RHITests` and `Reflector` already use a portable `int main`, so they needed nothing.
- [ ] **Editor** — `UI/EditorUI.cpp` includes `<Windows.h>` directly;
      `Tools/Screenshot/ScreenshotCapture.cpp` and `UI/Tools/Debug/AboutEditorTool.cpp` have
      `_WIN32` branches.

---

## Tier 3 — Language and ABI issues GCC/Clang will surface

Things MSVC accepts that the other compilers will not, plus places where the existing GCC/Clang
branch is present but wrong.

- [x] **`TCHAR`. Decided: wide on Windows, narrow UTF-8 everywhere else.** `WIDECHAR` stays
      `wchar_t` on both (UTF-16 on Windows, UTF-32 on Linux) and survives only for the Win32 API
      boundary; `TCHAR` collapses onto the engine's real string type everywhere else, so a
      `TEXT()` literal is a compile-time constant rather than a runtime widen-then-narrow.
      `PLATFORM_TCHAR_IS_WIDE` is the switch for the few places that genuinely cannot be written
      once.

      Windows was deliberately left wide rather than being narrowed too, because `winnt.h`
      redefines `TEXT` **unconditionally**: a narrow engine definition would silently flip back to
      wide partway through every translation unit that includes `windows.h`. Going fully narrow
      later is still open, but it means renaming the engine macro, not just changing a typedef.

      A `static_assert` in `GenericPlatform.h` now fails the build if `TEXT()` and `TCHAR` ever
      disagree on width — otherwise the symptom is the wrong character type reported at hundreds of
      call sites instead of at the cause. Verified that it actually fires.

      Two overloads had to change, both cases where the wide and narrow signatures collapse into
      one: `FName(const TCHAR*)` is now guarded, and `FName(const FFixedWString&)` converts
      explicitly instead of delegating through a bare wide pointer, which is ambiguous between
      `FWString` and `FFixedWString` once the `TCHAR` overload is gone. `FromWideString` gained a
      pointer overload to make that expressible.
- [x] `PLATFORM_WIDECHAR_IS_CHAR16` is now defined rather than relied on as an undefined
      identifier. It stays 0 — `WIDECHAR` is `wchar_t` on every platform — so the branch was right
      by accident before, and is right by decision now.
- [ ] `PLATFORM_UNIX` gates `LIKELY`/`UNLIKELY` in `Platform.h` but is never defined by any
      `.Build.cs`, so branch hints would silently degrade to no-ops on Linux.
- [x] `FORCEINLINE` expanded to `__attribute__((always_inline))` **without `inline`**. Not a
      warning — a link failure. `__forceinline` implies `inline`; `always_inline` does not, so every
      header-defined function carrying it got external linkage in each translation unit. Reproduced
      with a two-TU link on both compilers (`LNK2005` under Clang, `multiple definition` under GCC),
      and both link clean with `inline` added. All 428 call sites' decl-specifier combinations
      (`constexpr`, `friend`, `static`, `explicit`, `operator`) verified under `-Wall -Wextra
      -Werror`.
- [x] `LUMINA_STDCALL` expanded to `__attribute__((stdcall))`. Confirmed against a real target:
      Clang on `x86_64-unknown-linux-gnu` reports *"'stdcall' calling convention is not supported
      for this target"*, once per translation unit, and this header reaches all of them. Now empty
      on x86-64 and still spelled out for 32-bit x86, where the convention is real — verified on
      both `x86_64-` and `i386-unknown-linux-gnu`. Note this makes `FVoidFuncPtr` a plain
      `void(*)()`, which it already was on Windows x64, since MSVC accepts and ignores `__stdcall`
      there. `FVoidFuncPtrCDecl` is consequently an exact duplicate of it, and unused — left alone
      rather than folded into this fix.
- [ ] `LUMINA_DISABLE_OPTIMIZATION` uses `#pragma GCC optimize`, which Clang ignores with a warning.
- [ ] `Platform/Windows/WindowsPlatform.h` defines `VARARGS __cdecl` / `STDCALL __stdcall` with no
      generic counterpart.
- [~] `GenericPlatform.h`: `SIZE_T`/`uint64` are `unsigned long long`; Linux `size_t` is
      `unsigned long`. Distinct types → overload failures and format-string warnings. Same for
      `int64` vs `int64_t`.

      **First real casualty found and fixed:** `FArchive` has an `operator<<(uint64&)` but no
      `size_t` one, and `Archiver.h` streamed five `size_t` locals. On Windows the two are the same
      type so it bound silently; on Linux nothing matched and every translation unit including the
      header failed. Those locals are now `uint64`, which also pins the serialized width — a
      `size_t` length field would have written a different number of bytes per architecture. No
      change to the Windows byte stream, since the types were already identical there.

      Expect more of these wherever a `size_t` meets an explicitly-sized overload set.
- [ ] **Include-path case sensitivity.** Linux filesystems are case-sensitive; `<Windows.h>` vs
      `<windows.h>` already appears both ways in the tree, so the habit is present. Expect this to
      be the largest volume of mechanical errors in the first real build.
- [x] **Fiber-safe TLS has a GCC/Clang equivalent, and it is not obvious.** `JobScheduler.cpp`
      compiles with `/GT` on MSVC because the compiler otherwise caches a `thread_local` block
      address across `Fibers::Switch`, and a fiber resuming on another OS thread then reads the
      previous thread's state — "passes at 2 workers, crashes at 16". GCC does the same thing;
      confirmed by reading the generated assembly:

      ```
      call __tls_get_addr@PLT     ; resolved once
      movq %rax, %rbx             ; cached in a callee-saved register
      call OpaqueSwitch@PLT       ; the fiber can resume on a DIFFERENT thread here
      movl (%rbx), %eax           ; reads the previous thread's block
      ```

      There is no `/GT` flag, but `-ftls-model=initial-exec` reaches the same place: what gets
      cached is then the *offset* from the thread pointer, identical on every thread, and the `%fs`
      prefix is applied by the CPU at execution time.

      ```
      movq TLS@gottpoff(%rip), %rbp
      movl %fs:0(%rbp), %ebx
      call OpaqueSwitch@PLT
      movl %fs:0(%rbp), %eax      ; correct after migration
      ```

      Applied per file in `Runtime.Build.cs`, mirroring `/GT`, and deliberately not per module:
      initial-exec allocates from the static TLS surplus, which a `dlopen`ed library can exhaust
      ("cannot allocate memory in static TLS block").
- [x] **`uint64` was spelled `unsigned long long`, and that was the single biggest source of Linux
      errors.** On Windows `unsigned long long`, `uint64_t`, `size_t` and `SIZE_T` are all the same
      type, so writing one and meaning another is invisible. On Linux LP64, `uint64_t` and `size_t`
      are `unsigned **long**` — a distinct type of the same width. Consequences, none of which any
      amount of Windows testing could surface:

      - `SIZE_T GetCapacity() override` silently stopped overriding a `size_t` base
      - Vulkan entry points taking `uint64_t*` rejected a `uint64*`
      - `FArchive::operator<<(uint64&)` no longer accepted a `size_t`
      - `Math::Max(uint64, 1024ull * 1024)` failed to deduce one `T`

      `uint64`/`int64` now derive from `<cstdint>`, restoring the coincidence the code assumes.
      **No-op on Windows.** A `ull` literal is still a different type from `uint64`, so the few
      places mixing them got a typed `kMegabyte` constant instead.
- [~] **MSVC's permissive parsing accepts a lot that GCC and Clang reject.** Each of these compiled
      on Windows for years:

      | Construct | Why it is wrong |
      |---|---|
      | `requires !std::is_same_v<...>` | a requires-clause takes a primary-expression; `!x` is not one |
      | `friend class CWorld;` used as a declaration | a friend declaration is not findable by ordinary lookup |
      | `ImPlotContext* ImPlotContext` | a member sharing its type's name changes what the name means (`-Wchanges-meaning`) |
      | `RootMotion::Fn()` defined inside `namespace RootMotion` | explicit qualification in a declaration |
      | `std::ceilf` | not a standard name; MSVC provides it as an extension |
      | `Forward<T>()` in a template, header not included | MSVC defers lookup; GCC resolves at definition time |
      | `FTileViewWidget*` before its declaration | MSVC finds the later definition |
- [~] **Two-phase name lookup.** `/permissive-` is already on, which removes most of it. Two real
      cases surfaced while compiling the platform layer with GCC, both fixed:
      `Containers/Name.h` wrote `struct eastl::hash<...>` *inside* `namespace eastl` (extra
      qualification, which MSVC accepts), and `Paths/Paths.h` omitted `typename` before the
      dependent `T::value_type` in a requires-clause. Expect more of both as coverage widens.
- [ ] **Unity blobs make missing includes latent.** Adding one `.cpp` to Runtime reshuffled the
      blobs and broke the *Windows* build: four Scripting files were reaching `CClass` only
      through a blob neighbour. Fixed by including `Core/Object/Class.h` where it is used. Worth
      knowing that any new source file can surface this anywhere in the module.
- [ ] **`operator new`/`delete` interposition.** `LuminaModuleRules` adds
      `GlobalAllocatorOverrides.cpp` and `EASTLImpl.cpp` as `PerImageSourceFiles`, on the explicit
      reasoning that each image binds its own copy. **That is a Windows property.** On ELF the
      first loaded definition wins process-wide, so the per-image scheme collapses into one and the
      rpmalloc per-module instance design needs rethinking (`-fvisibility=hidden` plus a version
      script, or a single owning image). Design-level, not mechanical.

Already handled — no work needed:

- `Core/DisableAllWarnings.h` has full Clang/GCC/MSVC branches.
- `Core/Assertions/Assert.h` `LUMINA_ASSUME` covers MSVC, Clang and GCC.
- `Platform.h` `ALIGN`, `FORCENOINLINE`, `LUMINA_NOVTABLE` have working non-MSVC branches.

---

## Tier 4 — Renderer and runtime

Better shape than expected.

- [x] **Vulkan surface creation goes through GLFW** (`glfwGetRequiredInstanceExtensions`,
      `glfwCreateWindowSurface`). No `VK_KHR_win32_surface` hardcoding in `VulkanRHI.cpp`.
- [x] **Platform identity is already parameterized** — `LUMINA_SYSTEM_NAME`,
      `LUMINA_PLATFORM_NAME`, `LUMINA_SHAREDLIB_EXT_NAME` are all fed from `TargetInfo`.
- [ ] **Shared library naming.** `IBuildPlatform.SharedLibraryPrefix` supports `lib`, but
      `ModuleManager.cpp` reduces `"Foo-Development.dll"` to `"Foo"` without prefix awareness, and
      `Engine.cpp` resolves `"<Name>-<Config>.dll"` next to the executable.
- [ ] **Runtime dependency staging** needs `$ORIGIN` rpath, since Linux has no
      executable-directory-first search.
- [ ] **GLFW on Wayland.** The Linux branch in `GLFW.Build.cs` selects `_GLFW_X11` only; XWayland
      covers it, native Wayland does not.

---

## Tier 5 — C# / LuminaSharp

Mostly fine.

- [x] `ManagedProjectStep` already picks `dotnet.exe` vs `dotnet` by host OS.
- [ ] `DotNetHost.cpp` hostfxr resolution and `.dll` extension assumptions.
- [ ] `LUMINA_SCRIPT_API = DLL_EXPORT`, which the Linux platform must define as
      `__attribute__((visibility("default")))` so `NativeLibrary.TryGetExport` still resolves the
      thunks under `-fvisibility=hidden`.

---

## Running it on a Linux machine

```bash
# 1. Prerequisites (Debian/Ubuntu). GCC 13+ is required: the tree needs <format>, and
#    Ubuntu 22.04's stock GCC 11 does not have it.
sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test
sudo apt-get update && sudo apt-get install -y g++-13 pkg-config \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxkbcommon-dev
# .NET 10 SDK: https://dotnet.microsoft.com/download/dotnet/10.0

export CXX=g++-13 CC=gcc-13      # or leave unset once g++ is 13+ by default

# 2. Dependency bundle. Needs libclang 19 present to build:
wget https://apt.llvm.org/llvm.sh && chmod +x llvm.sh && sudo ./llvm.sh 19
sudo apt-get install -y libclang-19-dev
BuildScripts/MakeLinuxBundle.sh          # prints a SHA-256 to pin in SetupMode.Bundles

# 3. Build, nearest milestone first.
./LuminaBuild.sh Build Reflector -TargetType=Program    # verified working
./LuminaBuild.sh Build Lumina -TargetType=Editor        # not yet reached
```

`Setup.sh` checks the prerequisites above and fetches the bundle once it is published; until then,
`MakeLinuxBundle.sh` plus extracting it into `External/` by hand is the route.

## Order

1. ~~Tier 0 build tool — `LinuxPlatform`, `ClangToolchain`, dependency-format dispatch, locator.~~
   **Done.** Verified additive: a Windows editor build after the change reports 0 of 265 actions
   out of date, so the MSVC command lines are byte-identical.
2. ~~Tier 0 rules cleanup — move MSVC flags behind platform checks.~~ **Done** for the two files
   that carried them.
3. ~~Decide the `TCHAR` question.~~ **Done.** Narrow on Linux, wide on Windows; both shapes
   syntax-checked against the real engine headers, Windows builds unchanged.
4. ~~Tier 1 dependency plumbing.~~ **Done** — per-platform bundles, tar extraction, sentinel
   probing, build rules, and an assembly script with layout verification.
5. **Next: run `MakeLinuxBundle.sh` on a Linux host and publish the asset.** Everything upstream of
   it is ready; this is the last thing standing between the tree and its first compiled Linux
   translation unit, and it needs a machine I do not have.
6. ~~Tier 2: `PlatformProcess`.~~ **Done and tested on a real Linux host.**
7. ~~Tier 2: threads.~~ **Done and tested.**
8. ~~Tier 2: module export macros, `DLLMain`, allocator shim exports.~~ **Done and tested.**
9. ~~Tier 2: `GUID`, `StdoutSink`.~~ **Done and tested.**
10. ~~Small batch: dialogs, RenderDoc, EditorUI, GLFW X11 libs.~~ **Done.**
11. ~~Fibers.~~ **Done and tested.**
12. ~~Entry point.~~ **Done and tested.**
13. ~~Directory watcher.~~ **Done and tested.**
14. **One item left before Runtime can link on Linux:**
    - **Crash handler** — `Install`/`Shutdown`/`ReportFatal`/`AddDiagnosticProvider` are
      Windows-only; the shared `Platform/CrashHandler.cpp` supplies only the dump-directory half.
      Medium; `sigaction` plus an alternate signal stack.
14. Then: `DotNetHost`'s `_WIN32` branches, and whatever the first full-tree compile shakes out —
    which, on the evidence so far (the `size_t`/`uint64` archiver break, the missing `typename`,
    the extra qualification), will not be nothing.

### First milestone reached: the Reflector builds, links and runs on Linux

Verified end to end in WSL (Ubuntu 22.04, GCC 13 from the ubuntu-toolchain-r PPA, libclang-19-dev):

```
Using GCC 13.4.0 (gold)
Building Reflector - Linux64 - Development
Reflector succeeded: 3 actions in 30.13s.
Output: Binaries/Linux64/Reflector
```

and it runs — an ELF 64-bit PIE binary that prints its own diagnostics and banner. That exercises
the whole Tier 0 stack for real: toolchain discovery, the platform registry, the rules assembly, the
compile and link command lines, and runtime-dependency staging.

Four bugs surfaced only by doing it, none of which review had caught:

- **`gcc-ar` does not support `@file`.** The archiver is deliberately `gcc-ar`/`llvm-ar` rather than
  plain `ar`, because plain `ar` cannot index a bitcode archive and an LTO Shipping build then fails
  at link. But those wrappers insert the LTO plugin and hand off, and they do not implement response
  files: `ar` reports `invalid option -- '@'` and prints its usage. Archive arguments are now passed
  directly, which a Linux `ARG_MAX` of ~2 MB makes safe. The link step keeps its response file,
  where the argument list is far longer and the compiler driver does support `@file`.
- **`StringHash.h` included `"String.h"`** — the first real case-sensitivity casualty. It resolved
  on Windows only because the EASTL include directory is on the path and `EASTL/string.h` matches
  case-insensitively. Now spelled `"EASTL/string.h"`, like the lines beneath it already were.
- **`EASTLConfig.cpp`'s `Vsnprintf16` had a non-MSVC branch that has never compiled.** It called
  `convertstring<>`, an EASTL *sample* helper the file never included, and `memcpy` without
  `<cstring>`. Dead code on Windows; the first non-MSVC build is what noticed.
- **The Reflector needs three Linux libraries, not one.** It uses Clang's C++ AST API
  (`<clang/AST/Decl.h>`), not just the libclang C API, so `clang-cpp` and `LLVM-19` are required
  alongside `clang`. The last is needed even though nothing includes an LLVM header: the clang C++
  headers reference `llvm::DisableABIBreakingChecks`, and the link fails on exactly that name.

Also fixed in `MakeLinuxBundle.sh`, found while staging: the distribution's `lib/libclang.so` is a
symlink pointing **out** of the LLVM tree (`../../x86_64-linux-gnu/…`), so the `cp -a` the script
used would have staged a dangling link. Each library is now resolved to its real object, copied
under its SONAME, and the plain `-l` name recreated as a local relative symlink.

**Known deployment gap, not yet fixed:** `AddRuntimeDependency` stages libclang as `libclang.so`,
but the binary records the SONAME `libclang-19.so.19`. On a machine that also has libclang installed
the loader finds the system copy and nothing looks wrong; on one that does not, the staged file will
not satisfy the dependency. Runtime dependencies with a SONAME need staging under that name.

**A Linux host is available via WSL** (`wsl.exe -d Ubuntu-22.04`), with GCC 13 installed from the
ubuntu-toolchain-r PPA — Ubuntu 22.04's stock GCC 11 has no `<format>` and cannot build this tree.

**Do not read build times from that setup as representative.** Building out of `/mnt/h` means every
header crosses WSL's 9p bridge to the Windows filesystem. Measured on ~200 EASTL headers:

| | wall time |
|---|---|
| `/mnt/h` (9p, Windows drive) | 1.270 s |
| same files on native ext4 | 0.003 s |

Roughly **400x**. The reflection step feels that hardest, since libclang opens thousands of headers:
the Reflector sat in uninterruptible I/O wait (`D` state) burning about one second of CPU per
fifteen of wall clock — over 90% blocked, not computing. The same build on native ext4 finished the
Reflector in **16 seconds**. Clone into the WSL filesystem, or build on a real Linux machine,
before drawing any conclusion about how slow something is.

**And more importantly, `/mnt` is case-INSENSITIVE**, because it is a Windows filesystem. Building
there silently accepts every wrong-case include, which is the single largest class of Linux porting
error. A build from `/mnt` that succeeds proves nothing about case correctness. Both case bugs found
so far were invisible there and failed immediately on ext4:

- `StringHash.h` including `"String.h"` (meaning `EASTL/string.h`)
- **33 includes across 8 first-party files spelling `<eastl/...>` in lowercase** when the directory
  is `EASTL/` — including most of `RuntimePCH.h`, so effectively the whole engine. `ls eastl/` even
  *appears* to work from Git Bash on Windows, for the same reason.
That is what made the verification above possible without the dependency bundle: a translation unit
can be compiled and run against stubs long before the engine links. Reuse the trick by taking the
`/I` and `/D` set out of a Windows `.rsp` under `Intermediates/Obj/Windows64/`, stripping
`LE_PLATFORM_WINDOWS`, `UNICODE` and `_UNICODE`, and adding `-mavx` (the tree defines `__AVX__`,
which GCC only honours with the flag).

`Platform.h` is now clean under `-Wall -Wextra -Werror` on `x86_64-unknown-linux-gnu`.

Remaining Tier 0, none of it blocking: project generation for Linux, `.sh` entry scripts,
`HostCapabilities`, and the case-insensitive path keys in `DependencyCache`/`FileItem`.
