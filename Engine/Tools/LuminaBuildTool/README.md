# LuminaBuildTool

The build and project generation system for Lumina Engine. It is the sole authority on how the
engine is configured, generated and compiled; Premake has been removed.

Modules and targets are described in C# rules files (`*.Build.cs`, `*.Target.cs`) that the tool
compiles and executes. From those it resolves a module dependency graph, derives compile and link
command lines, runs the reflection generator, decides what is out of date, runs the toolchain, and
writes IDE project files.

## Running it

`LuminaBuild.bat` at the engine root is the front end. It rebuilds the tool if needed and forwards
everything else:

```bash
LuminaBuild.bat Build Lumina -TargetType=Editor -Configuration=Development
```

| Script | Purpose |
| --- | --- |
| `Setup.bat` | First-time setup: prerequisites, dependency bundle, project files |
| `GenerateProjectFiles.bat` | Refresh the IDE solution |
| `LuminaBuild.bat` | Everything else, forwarded to the tool |

Modes:

| Mode | Purpose |
| --- | --- |
| `Setup` | Fetch and verify the external dependency bundle, persist LUMINA_DIR, set git hooks |
| `Build <Target>` | Compile and link a target |
| `Clean [Target]` | Delete a target's outputs, or every intermediate |
| `Query [Target]` | List targets, modules and plugins, or describe one resolved target |
| `Includes <Target>` | Rank headers by how many translation units include them |
| `Deps <Target>` | Compare a module's declared dependencies against the ones it reaches |
| `GenerateProjectFiles` | Write `.vcxproj`, `.filters`, a solution and `compile_commands.json` |

Targets: `Lumina` (the editor and game launcher), `Reflector` (the reflection generator),
and `Tests`. Game projects live outside the engine tree and are passed with `-Project=<path>`.

Common options: `-Configuration=Debug|Development|Shipping`, `-TargetType=Editor|Game|Program`,
`-Platform=Win64`, `-EngineRoot=<path>`, `-Project=<path>`, `-MaxParallel=<n>`, `-DryRun`,
`-Clean`, `-KeepGoing`, `-RecompileRules`, `-Verbose`, `-Trace`, `-Timeline`,
`-NoProjectFileUpdate`.

## Measuring the build

`-Timeline` writes a Chrome Trace Event file to
`Intermediates/BuildTool/Timeline-<Target>-<Type>-<Configuration>.json`, which opens in Perfetto or
`chrome://tracing`, and logs the ten longest actions. Spans cover execution only, not the wait for a
parallelism slot, so a gap in the picture is real idleness rather than queueing. The lane count the
viewer shows is the parallelism the build achieved, which is the number worth comparing against the
one it was allowed.

`Includes` and `Deps` read the header closure the compiler reported during the last build, which
LuminaBuildTool already keeps so that editing a header rebuilds exactly the objects that read it.
Both need that target to have been built once; neither compiles anything itself.

```bash
LuminaBuild.bat Includes Lumina -Top=30
LuminaBuild.bat Includes Lumina -Module=Runtime
LuminaBuild.bat Deps Lumina
```

`Includes` rolls the ranking up to the module owning each header before listing individual files,
because a library is the unit you make a decision about. A header high in that list and outside the
precompiled header is the cheapest clean-build win available; one low in it and inside a PCH is
weight every translation unit pays and almost none use. `-All` adds toolchain and SDK headers, which
are excluded by default because they dominate the ranking and cannot be changed.

`Deps` reports two things. Declared dependencies whose headers a module never opens are candidates
for deletion, though not confirmed ones: a dependency can exist purely to link. Headers reached from
an undeclared module compile today only because something else re-exports them, and the report names
the module they arrive through, since that is the edge whose removal would break the build. Note that
a per-image source such as `EASTLImpl.cpp` is compiled into every module and its closure is charged
to whichever module compiled it, which accounts for entries with no re-exporting module named.

## Module layering

Every resolved graph is checked against the layering the rules declare, before any compile or link
environment is built. All violations are reported at once, because architectural drift is usually
several edges that arrived together.

Two checks need no declaration. A third-party module must not depend on a first-party one: vendored
code has to stand alone so it can be replaced by the next version of itself, and an edge back into
the engine turns that update into a merge. And a target's own `ForbidDependency` rules are checked
across the whole closure, so routing a forbidden edge through an intermediate module does not evade
it; the error names the shortest path it found.

```csharp
ForbidDependency(
    "Runtime",
    "Editor",
    "The runtime is what ships. An editor dependency here cannot link in a Game target.");
```

The reason is required, and it is quoted back in the error. A layering rule without one becomes
folklore as soon as whoever added it stops answering questions about it.

Host type is deliberately **not** checked. "A dependency must exist everywhere its dependent does"
reads like the obvious rule and is unsound: a `Build.cs` is evaluated per target type, so a module
can name an editor dependency inside a `Target.bWithEditor` check and the edge simply does not exist
in a Game resolution. `Lumina` does exactly that. What is left after excluding conditional edges is
a dependency missing from the target being resolved right now, which module resolution already
rejects with a better message.

A target that genuinely has to build against its own declared layering sets
`bEnforceModuleLayering = false` in its rules. There is no command-line equivalent on purpose: a
guard any build can wave away stops being one the first time waving it away is quicker than fixing
the dependency.

## Compile database

Project generation also writes `compile_commands.json` at the workspace root, where clangd,
clang-tidy and editors other than Visual Studio look for it. The commands come from the same
toolchain call the build uses, so a tool sees what the compiler sees.

Two differences from the real command line, both about precompiled headers. Clang cannot read an
MSVC `.pch`, so `/Yc` and `/Yu` become a forced include of the same header and `/Fp` is dropped: the
same declarations arrive by the route that does not need a binary the tool cannot parse.
`/sourceDependencies` is dropped because it asks for a build artifact and reading code is not a
build. Entries are per file rather than per unity blob, because a blob's command line does not name
the sources it absorbed.

The file is generated, machine specific and gitignored.

## Writing rules

A module is a directory containing a `<Name>.Build.cs`:

```csharp
using LuminaBuildTool.Configuration;

public class Renderer : LuminaModuleRules
{
    public Renderer(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.SharedLibrary;

        // Exposed through this module's own public headers, so dependents see them too.
        PublicDependencyModuleNames.Add("Runtime");

        // Implementation only; stops here.
        PrivateDependencyModuleNames.AddRange(new[] { "Volk", "VMA" });

        PublicIncludePaths.Add("Public");
        PrivateDefinitions.Add("LUMINA_RENDERER_VULKAN");

        if (Target.bWithEditor)
        {
            PrivateDependencyModuleNames.Add("Editor");
        }
    }
}
```

A target is a `<Name>.Target.cs`:

```csharp
public class LuminaTarget : LuminaTargetRules
{
    public LuminaTarget(TargetInfo Target)
        : base(Target)
    {
        Type = Target.Type;
        LaunchModuleName = "Lumina";
        EnabledPlugins.Add("GameplayExtras");
    }
}
```

The class name must match the file's base name, optionally with a `Target` or `Module` suffix.
Constructors take a single `TargetInfo`, and may call `ModulePath()` / `TargetPath()` to resolve
paths relative to the rules file.

Shared helper code goes in a `*.BuildRules.cs` file. It is compiled into the rules assembly but
never instantiated as a module. `Engine/Build/Lumina.BuildRules.cs` holds the engine-wide target
defaults and the `LuminaModuleRules` / `LuminaThirdPartyModuleRules` base classes.

### Public versus private

Public settings propagate transitively through `PublicDependencyModuleNames`. Private settings
apply to the declaring module only. Use public for anything a module's own headers expose, and
private for everything else; that split is what keeps include paths from leaking across the graph.

### Plugins

A plugin is a directory containing a `.lplugin` descriptor. The descriptor is the single source of
truth for the plugin's identity and its module list, and each listed module needs a matching
`Build.cs` under the plugin's `Source` tree. Plugin binaries are written to the plugin's own
`Binaries/<Platform>` directory so the runtime plugin loader finds them where it expects.

A plugin is built when `EnabledByDefault` is set in its descriptor, or when a target lists it in
`EnabledPlugins`. `DisabledPlugins` overrides both. `EditorOnly` plugins and `Editor` host-type
modules are dropped from Game targets. Standalone `Program` targets host no plugins at all.

## How incremental builds decide

Every action declares its inputs and its outputs. An action reruns when:

1. an output is missing,
2. the command that produced that output differs from the recorded one,
3. a declared input is newer than the outputs, or
4. a header the previous compile actually included is newer than the outputs.

Outdatedness then propagates to dependents.

Rule 2 is what makes a `Build.cs` or `Target.cs` edit invalidate the right work, and only the
right work. Rules changes reach the build as different compiler and linker command lines, so
adding a public definition to a leaf module rebuilds that module and everything downstream of it,
while an edit with no effect on any command line rebuilds nothing. This is more precise than
invalidating on the rules file's timestamp, which would rebuild the world on any edit.

Rule 4 uses the compiler's own `/sourceDependencies` output, so header tracking reflects what was
really included rather than what the rules claimed.

Caches live under `Intermediates/Obj/<Platform>/<Type>-<Config>/.buildtool/`. Deleting them costs
a rebuild and nothing else.

## Unity builds

A module can compile its sources as a few generated files that `#include` the real ones, so a
shared header is parsed once per group instead of once per source.

It is on by default. `TargetRules.bUseUnityBuild` sets the default for a target;
`ModuleRules.bUseUnityBuild` is a nullable override, so a module only has to say something when it
disagrees.

Vendored libraries are merged too, but individually rather than as a category: of 33 third-party
modules, 27 merge and 6 are opted out in their own `Build.cs` with the error that made them opt
out. Five fail to compile when merged, each in a way that is characteristic of the technique:

| module | why |
| --- | --- |
| `BasicUniversal` | `astc_6x6_hdr` becomes ambiguous across merged sources |
| `ImGui` | the Vulkan backend expects to be the TU that includes the Vulkan headers |
| `MSDFGen` | `_USE_MATH_DEFINES` only affects the first `<cmath>` include, so `M_PI` vanishes |
| `Recast` | file-scope names collide, binding an initializer to a function |
| `RmlUi` | `CommonSource.h` defines non-inline variables at namespace scope |

The sixth, `EA`, is different and worth knowing about: it compiles and then fails to *link*. A
static library lets the linker take only the objects it needs, and EASTL depends on that, since the
engine supplies its own `eastl::AssertionFailure` in `EASTLImpl.cpp`. Merging puts EASTL's copy in
the same object as symbols the engine does reference, so it is always pulled in and collides.
Merging changes link granularity, not just compilation, and a compile-only sweep will not find it.

What it costs is isolation. Sources in one unity file share a translation unit, so a file-scope
static, an anonymous namespace, a `using namespace` or a macro left defined reaches its neighbors,
and two files that each defined the same internal helper now collide. Turning it on for the engine
surfaced four such collisions in `Runtime` and four in `Editor`; each one was duplicated code or a
name shadowing an engine symbol, and all were fixed in the sources rather than worked around here.
A module that genuinely cannot take it sets `bUseUnityBuild = false` and says why.

Some sources can never be merged, and those are held back automatically rather than by anyone
remembering to list them:

- anything with `PerFileCompilerOptions`, which would otherwise silently lose the flag it asked
  for (`JobScheduler.cpp` needs `/GT`, and losing it is a crash, not an error),
- the precompiled header source, which has to run `/Yc` in its own translation unit,
- `PerImageSourceFiles`, which are per loaded image rather than per module,
- the reflection shards, which are already generated as unity files.

`ModuleRules.ExcludeFromUnity` covers what only the code knows: a source carrying a single-header
library's implementation macro must be the only translation unit that does, so `StbImageImpl.cpp`
and the plugin's `RymlImpl.cpp` name themselves there.

Grouping is by source bytes (`UnityBuildBytesPerFile`, 384 KB) rather than a file count, because
sizes within a module vary by orders of magnitude. Files are sorted before grouping, so the same
tree produces the same groups on any machine and an untouched module keeps the objects it has.
Modules with fewer than `MinFilesForUnityBuild` mergeable sources compile file by file.

Each generated file declares its members as build inputs, so editing a merged source rebuilds the
group that holds it on the very first build, before any header scan has run. Generated IDE
projects list real sources and never these files.

`-NoUnity` compiles everything file by file for one build, which is the first thing to try when a
compile error appears only in a unity configuration.

Measured on a clean Development Editor build: 590 actions in 77.3s with unity, 1035 actions in
103.2s with `-NoUnity`.

## Architecture

```
Core/           Logging, command line, interned file handles with cached timestamps, hashing
Configuration/  The rules API: TargetRules, ModuleRules, PluginDescriptor, directory layout
Rules/          Discovery of rules files and their Roslyn compilation into a cached assembly
Graph/          Source discovery, module and target resolution, dependency propagation, actions
Toolchain/      IToolchain plus the MSVC implementation and its compiler and SDK locators
Platform/       IBuildPlatform: naming conventions and toolchain selection per platform
Execution/      Action graph, timestamp and command caches, parallel executor
ProjectFiles/   IProjectFileGenerator plus the Visual Studio implementation
Modes/          Build, Clean, Query and GenerateProjectFiles entry points
```

The boundaries that matter:

- **Nothing outside `Toolchain/` knows a compiler flag.** The graph deals in include paths,
  definitions and libraries; the toolchain turns those into command lines.
- **Nothing outside `Platform/` knows a file extension.** Adding a platform means implementing
  `IBuildPlatform` and `IToolchain` and registering the platform, with no core changes.
- **`Execution/` does not know what an action means.** It knows inputs, outputs and a command,
  which is why the same executor handles compiling, archiving, linking and code generation.
- **`Rules/` is the only place that reflects over user types.** Everything downstream works with
  resolved data.

## Generated IDE projects

Projects are NMake projects: building one from the IDE calls back into LuminaBuildTool rather than
reimplementing the build in MSBuild.

There are three kinds, and the split matters:

- **Target projects** (`Lumina`, `Reflector`, `Tests`) build. One project drives one
  target, which is what keeps a parallel solution build from starting several tool instances
  against the same output.
- **Module projects** are for browsing and editing. Each carries its own module's include paths,
  definitions and force-includes so IntelliSense is exactly right, and builds nothing: a module is
  not independently buildable, and giving it the target's build command is what previously made a
  solution build launch one full build per module.
- **`LuminaRules`** is a C# project holding every `Target.cs`, `Build.cs` and `BuildRules.cs`.
  Rules files are ordinary C# compiled against this tool, but an IDE only treats them that way if
  some project claims them; without one they open as plain text with no highlighting, no completion
  on `ModuleRules` and no error until a build fails. It is mapped into the solution with an
  `ActiveCfg` but no `Build.0`, so it never builds: the build system compiles the same files itself,
  and building them twice could fail for reasons a real build does not care about. Its settings
  mirror `RulesCompiler` deliberately, including `ImplicitUsings=disable`, so what the editor
  reports and what a build reports are the same thing.

A target sets `bBuildByDefault = false` to stay out of whole-solution builds while remaining
individually buildable; `Tests` does, so Build Solution builds the engine and the
reflection generator rather than everything.

### Running from the IDE

A solution file has no startup-project field, so the IDE runs whichever project is listed first
until the user picks another. The target that sets `bIsStartupTarget` is written first, and the
rest of the buildable targets follow, so run and debug land on a real target rather than on
whichever module sorted first alphabetically.

What running a target means is the target's own decision, because it is not always "launch what
this target built":

| | `DebuggerCommand` |
| --- | --- |
| Default | the launch module's executable |
| Game project | the editor, with `--Project=<name>.lproject` |

A game target builds a library the editor loads, so launching that library would be meaningless.
`LuminaGameTargetRules` points it at the engine instead and `SetProjectFileToOpen` supplies the
project. Settings are written to both the `.vcxproj` and a generated `.vcxproj.user`, which is
where Visual Studio and Rider read them from.

Independently of project shape, concurrent builds that would write the same output serialize on a
lock file rather than racing. The lock is keyed on the output root, platform, target type and
configuration, so different configurations still build in parallel. It is a file handle rather
than a named mutex because the build holds it across awaits and the operating system has to
release it if a build dies.

## Reflection

Modules with `bEnableReflection` are collected into one generator run per target. The tool writes
the Reflector's input document, declares every reflected header as an input and every generated
unity shard as an output, and lets the ordinary action graph sequence it: the shards are compile
inputs, so nothing needs to know that a generator produced them.

Because the Reflector rewrites a generated file only when its content changes, a header edit that
changes no reflected type regenerates nothing, and the compiles that would have followed are
dropped at execution time rather than run pointlessly. Editing one engine header rebuilds the
handful of translation units that included it, not the module.

Generated C# bindings default to `Intermediates/CSharpBindings`, which compiles them into
`LuminaSharp`. A plugin or game module sets `CSharpBindingsDirectory` to route them into its own
script assembly instead.

Generated C++ is per target, under
`Intermediates/Reflection/<Platform>/<Target>/<Type>-<Config>/<Module>`, for the same reason
object files are: two targets that share a module do not necessarily generate the same code for
it, and one shared directory makes each build look stale to the other. The build passes that path
to the generator explicitly, so the layout is the build system's decision rather than a convention
baked into the tool.

## Optional features

`Engine/Build/BuildConfiguration.json` sets each feature to `auto`, `on` or `off`, and a command
line switch of the same name overrides it. What `auto` means per feature is engine policy and
lives in `Engine/Build/LuminaFeatures.BuildRules.cs`, not in the tool.

```bash
LuminaBuild.bat Build Lumina -TargetType=Editor -Tracy=off
```

Because feature state reaches the build as preprocessor definitions, flipping one invalidates
exactly the work whose command lines changed.

## Game projects

A project builds against the engine tree rather than a prebuilt copy of it:

```bash
LuminaBuild.bat Build MyGame -Project=C:\Path\To\MyGame
```

Engine modules keep their intermediates and binaries in the engine tree, so the engine is built
once and shared by every project, while project modules land in the project's own `Binaries` and
`Intermediates`. Building a game project therefore also keeps the engine current instead of
silently linking against a stale one.

Project modules derive from `LuminaGameModuleRules` and project targets from
`LuminaGameTargetRules`, both in `Engine/Build/LuminaGameProject.BuildRules.cs`.

`GenerateProjectFiles -Project=<path>` writes the project's own solution, and the NMake commands it
emits carry that `-Project` through, so building from the project's IDE resolves the project's
targets and not just the engine's.

### Module export macros

`<MODULENAME>_API` is defined by the build system, not written down anywhere. Every module gets one
for itself and for every shared library in its dependency closure, resolved from the graph:
`DLL_EXPORT` while compiling the module that owns it, `DLL_IMPORT` for everything that consumes it,
and empty under a monolithic link.

This is what makes a game or plugin module able to export anything at all. A list of macros in an
engine header could only ever name the modules that shipped with the engine, and nothing out of
tree can add itself to it.

## Layout conventions

Source files are discovered by walking the module's source directories. A subdirectory containing
its own `Build.cs` belongs to another module and is skipped, so modules never fight over sources.
A module's source root is part of its public surface: dependents include its headers at the path
they sit at. Third-party modules declare their include roots explicitly instead.

Binaries land in `Binaries/<Platform>` using the platform's directory name, `Windows64` rather
than `Win64`. That string is baked into the engine as `LUMINA_PLATFORM_NAME` and is what the
runtime uses to resolve plugin and project module DLLs, so it cannot be changed on one side alone.

### Precompiled headers

A module's PCH is named after the module: `RuntimePCH.h`, `EditorPCH.h`. Because a module's source
root is on the include path of everything downstream, a generic name there is a name every
dependent then has to avoid. When both were `pch.h`, `EditorPCH.h`'s `#include "pch.h"` resolved to
the includer's own directory and Runtime's never arrived, which is why the Editor's had to be
renamed rather than the other way round.

No translation unit needs to include its PCH: the toolchain force-includes it ahead of everything
else, which is also what lets `/Yu` find its marker in a generated file nobody wrote by hand. The
explicit include at the top of each source is convention, and it has to name the module's own PCH.

The same applies to generated code. The reflection generator is told each module's PCH through the
reflection input document and emits that, or nothing when the module has none. It used to emit a
literal `pch.h`, which only ever worked because Runtime happened to be on everyone's include path.

### Rules that name a file

`PerFileCompilerOptions` and `ExcludeFromUnity` are keyed by bare file name, so a rename would
silently drop whatever was attached to the old one. The scanner fails the build when either names a
file the module does not have. Silence is the wrong outcome here: `JobScheduler.cpp` losing `/GT`
is a fiber-scheduler crash rather than a diagnostic.

## Current state

Everything the engine needs is in place: rules discovery and compilation, module and plugin graph
resolution with public and private propagation, MSVC toolchain detection, C and C++ compilation
with precompiled headers, reflection code generation, static library archiving, shared library and
executable linking, monolithic Shipping with whole-archive module registration, .NET project
builds, runtime dependency staging, header-accurate incremental builds, parallel execution, and
Visual Studio project and solution generation.

The engine builds through it in every configuration and target type, as do `Reflector` and `Tests`.

Verified: every target builds and then reports zero outdated actions on the next run, across
`Lumina` in Editor and Game for all three configurations, plus `Reflector` and `Tests`.

Known scope limits:

- Windows and MSVC only. Adding a platform means implementing `IBuildPlatform` and `IToolchain`
  and registering it; nothing in the core needs to change.
- Editor targets are never monolithic. `Runtime` and `Editor` each carry their own copy of the
  stb_image_write implementation, which is correct in separate images and a duplicate symbol in
  one. A shipping game, which excludes the editor modules, links monolithically as intended.
- The outdated-action count printed before a build is an upper bound. Actions whose inputs turn
  out not to have changed are dropped as the build runs, and the summary reports how many.
- The first build after generated code is created records the input high-water marks that make it
  current, so a freshly generated target settles on the second run rather than the first.
