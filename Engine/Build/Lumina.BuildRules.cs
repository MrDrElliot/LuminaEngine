using System.IO;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Platform;

/// <summary>Engine-wide target defaults.</summary>
public abstract class LuminaTargetRules : TargetRules
{
    protected LuminaTargetRules(TargetInfo Target)
        : base(Target)
    {
        CppStandard = "c++latest";

        // The launcher checks for AVX2 before anything else runs and explains the failure on an older CPU.
        VectorExtensions = "AVX2";

        bEnableExceptions = true;
        bEnableRtti = false;
        bUseDynamicCrt = true;

        string ConfigurationElement = Target.Configuration switch
        {
            BuildConfiguration.Debug => "-Debug",
            BuildConfiguration.Development => "-Development",
            _ => "-Shipping",
        };

        // Editor and Game link different exports from the same modules, so a shared file name would let the last build overwrite the other.
        OutputSuffix = Target.Type == TargetType.Game
            ? ConfigurationElement
            : $"-{Target.Type}{ConfigurationElement}";

        bDebugSymbols = Target.Configuration != BuildConfiguration.Shipping;
        bLinkTimeCodeGeneration = Target.Configuration == BuildConfiguration.Shipping;
        bIncrementalLinking = Target.Configuration != BuildConfiguration.Shipping;

        // Nobody measures performance in Debug, and forcing these there costs build time and step-through.
        ForceInlineHint = Target.Options.GetMode(LuminaFeatures.ForceInlineHint) switch
        {
            FeatureMode.On => ForceInlineHintPolicy.Force,
            FeatureMode.Off => ForceInlineHintPolicy.Hint,
            _ => Target.Configuration == BuildConfiguration.Debug
                ? ForceInlineHintPolicy.Hint
                : ForceInlineHintPolicy.Force,
        };

        GlobalDefinitions.AddRange(new[]
        {
            "_SILENCE_CXX23_ALIGNED_UNION_DEPRECATION_WARNING",
            "_SILENCE_CXX23_ALIGNED_STORAGE_DEPRECATION_WARNING",
            "IMGUI_DEFINE_MATH_OPERATORS",
            "IMGUI_IMPL_VULKAN_USE_VOLK",
            "RMLUI_STATIC_LIB",
            "__AVX__",

            // The runtime resolves plugin and project binaries from these, so they must stay Windows, 64, Windows64 and .dll.
            $"LUMINA_SYSTEM_NAME=\"{Target.SystemName}\"",
            $"LUMINA_ARCH_NAME=\"{Target.ArchitectureName}\"",
            $"LUMINA_PLATFORM_NAME=\"{Target.PlatformName}\"",
            $"LUMINA_CONFIGURATION_NAME=\"{Target.Configuration}\"",
            $"LUMINA_SHAREDLIB_EXT_NAME=\"{BuildPlatformRegistry.Get(Target.Platform).SharedLibraryExtension}\"",
            $"LUMINA_SHAREDLIB_PREFIX_NAME=\"{BuildPlatformRegistry.Get(Target.Platform).SharedLibraryPrefix}\"",
            $"LUMINA_EXECUTABLE_EXT_NAME=\"{Target.Platform.GetExecutableExtension()}\"",
        });

        LuminaFeatures.ApplyDefinitions(Target, GlobalDefinitions);

        GlobalDefinitions.Add(Target.Configuration switch
        {
            BuildConfiguration.Debug => "LE_DEBUG",
            BuildConfiguration.Development => "LE_DEVELOPMENT",
            _ => "LE_SHIPPING",
        });

        // Must agree with the CRT the toolchain selects, so a target with prebuilt release libraries pins a non-debug configuration.
        GlobalDefinitions.Add(Target.Configuration == BuildConfiguration.Debug ? "_DEBUG" : "NDEBUG");

        // Strips the API export and import macros and makes IMPLEMENT_MODULE register into a static table.
        if (Target.bMonolithic)
        {
            GlobalDefinitions.Add("LUMINA_MONOLITHIC");
        }

        if (Target.Platform == BuildPlatform.Windows64)
        {
            GlobalCompilerOptions.Add("/bigobj");
        }

        ConfigureWarnings(Target);

        // Checked across the whole closure, so routing through an intermediate module does not get past it.
        ForbidDependency(
            "Runtime",
            "Editor",
            "The runtime is what ships. An editor dependency here is not only a layering inversion, "
            + "it cannot link at all in a Game target, where the editor module does not exist.");

        // Vendored SDKs stay contained in their plugin so disabling it takes the SDK along.
        ForbidDependency(
            "Runtime",
            "NsightPerf",
            "The Nsight SDK belongs to the NsightPerf plugin. Nothing in the runtime should be built "
            + "against a vendor profiler that a project is free to disable.");
    }

    /// <summary>
    /// The engine's warning policy, in one place. Fatal means the class is a bug rather than a style
    /// preference, so it is worth failing the build over. Off is reserved for warnings that fire on
    /// code we do not own or cannot express differently.
    /// </summary>
    private void ConfigureWarnings(TargetInfo Target)
    {
        Warnings.Set(WarningSeverity.Fatal,
            CompilerWarning.ReturnType,             // falling off the end of a non-void function
            CompilerWarning.SequencePoint,          // unsequenced modification, the Memory.h NewArray bug
            CompilerWarning.StrictAliasing,         // type-punning the optimiser is allowed to break
            CompilerWarning.ClassMemAccess,         // memset or memcpy over a non-trivial type
            CompilerWarning.DeleteNonVirtualDtor,   // deleting through a base with no virtual destructor
            CompilerWarning.Reorder,                // initialisers that do not run in the order written
            CompilerWarning.NonNullCompare,         // testing a reference or nonnull parameter for null
            CompilerWarning.TautologicalCompare,    // a comparison whose result is fixed at compile time
            CompilerWarning.Parentheses,            // assignment or precedence that reads as something else
            CompilerWarning.Format,                 // printf-family arguments that do not match the format
            CompilerWarning.FormatTruncation);      // a formatted write the destination cannot hold

        Warnings.Set(WarningSeverity.Off,
            // Generated code takes offsetof of reflected types that are rarely standard layout, and it cannot be rewritten to avoid that.
            CompilerWarning.InvalidOffsetof,

            // The interference size is already pinned in Thread.h, which removes the hazard this warns about.
            CompilerWarning.InterferenceSize,

            // The -isystem headers still raise GCC's post-inlining diagnostics, with nothing to fix on our side.
            CompilerWarning.DanglingPointer);

        // Flow-sensitive analysis moves with the optimizer, so these stay visible but not fatal in Shipping or LTO builds.
        Warnings.Set(WarningSeverity.Warning,
            CompilerWarning.ArrayBounds,            // an index the optimiser can prove is out of range
            CompilerWarning.StringopOverflow,       // a mem/str call writing past the destination
            CompilerWarning.StringopTruncation,     // a copy the destination silently cuts short
            CompilerWarning.Restrict,               // arguments that alias where the callee says they cannot
            CompilerWarning.UseAfterFree,           // a pointer read after the block was released
            CompilerWarning.FreeNonheapObject,      // free or delete applied to something never allocated
            CompilerWarning.DanglingReference,      // a reference bound to a temporary that has already gone
            CompilerWarning.Uninitialized,          // union layouts trip this after inlining
            CompilerWarning.MaybeUninitialized,     // a local the optimizer thinks is read on a path that never wrote it
            CompilerWarning.DeprecatedDeclarations); // a compiler or SDK bump must not break the build

        if (Target.Platform == BuildPlatform.Windows64)
        {
            // Noise inherent to exporting C++ types across a DLL boundary.
            Warnings.Set(WarningSeverity.Off,
                CompilerWarning.DllInterface,
                CompilerWarning.DllInterfaceBase);

            // A narrowing conversion is a silent wrong answer rather than untidiness.
            Warnings.Set(WarningSeverity.Fatal,
                CompilerWarning.InconsistentDllLinkage,  // a definition declared with another module's export macro
                CompilerWarning.ConversionLoss,          // a value that does not survive its target type
                CompilerWarning.ConversionSizeT);        // a 64-bit count truncated into a 32-bit one
        }
        else
        {
            // Left out on MSVC until verified there, since these are off by default for cl and promoting them enables them.
            Warnings.Set(WarningSeverity.Fatal,
                CompilerWarning.Switch,

                CompilerWarning.OverloadedVirtual,       // an overload set a derived class silently narrowed
                CompilerWarning.NonVirtualDtor,          // a base deleted through a pointer it cannot destroy
                CompilerWarning.SuggestOverride,         // an override that no longer has to match the base

                CompilerWarning.ReturnLocalAddr,         // a pointer to a frame that has already gone
                CompilerWarning.MismatchedNewDelete,     // new[] released with delete, or the reverse
                CompilerWarning.PlacementNew,            // constructing into storage too small to hold it

                CompilerWarning.SizeofPointerMemaccess,  // sizeof of the pointer where the object was meant
                CompilerWarning.SizeofPointerDiv,        // an element count divided out of a pointer size
                CompilerWarning.SizeofArrayArgument,     // sizeof of an array parameter, which is a pointer
                CompilerWarning.MemsetEltSize,           // a byte count that ignores the element size
                CompilerWarning.MemsetTransposedArgs,    // memset's fill and length passed the wrong way round
                CompilerWarning.Nonnull,                 // null handed to a parameter declared non-null

                CompilerWarning.Address,                 // an address tested for truth instead of its value
                CompilerWarning.BoolOperation,           // arithmetic on a bool that cannot mean what it reads as
                CompilerWarning.LogicalNotParentheses,   // !x == y, which negates before it compares
                CompilerWarning.MisleadingIndentation,   // a statement whose indentation implies a block
                CompilerWarning.IntInBoolContext,        // an integer expression used as a condition
                CompilerWarning.EmptyBody,               // an if or loop whose body is a stray semicolon
                CompilerWarning.InitSelf,                // a variable initialised from itself
                CompilerWarning.SelfMove,                // a move onto the object being moved from
                CompilerWarning.ImplicitFallthrough,     // a case that runs into the next without saying so
                CompilerWarning.DuplicatedCond,          // an else-if that repeats a condition already taken
                CompilerWarning.DuplicatedBranches,      // a condition whose two branches are the same code
                CompilerWarning.LogicalOp,               // && or || where the bitwise operator was meant
                CompilerWarning.CatchValue,              // a polymorphic exception caught by value and sliced

                CompilerWarning.PessimizingMove,         // a move that blocks the copy elision it displaces
                CompilerWarning.RedundantMove,           // a move the compiler was already going to make
                CompilerWarning.PointerArith,            // arithmetic on void* or a function pointer
                CompilerWarning.Attributes,              // an attribute the compiler is quietly dropping
                CompilerWarning.Comment,                 // a // comment continued by a trailing backslash
                CompilerWarning.ExtraSemi,               // a semicolon that declares nothing
                CompilerWarning.SignCompare);            // a signed and an unsigned value compared directly

            Warnings.Set(WarningSeverity.Warning,
                CompilerWarning.UnusedVariable,
                CompilerWarning.UnusedButSetVariable,
                // Not clean on GCC 16.2, which is newer than the pass above was verified against.
                CompilerWarning.Shadow,
                CompilerWarning.SubobjectLinkage,
                CompilerWarning.UnusedFunction);
        }
    }
}

/// <summary>Defaults shared by every engine module.</summary>
public abstract class LuminaModuleRules : ModuleRules
{
    protected LuminaModuleRules(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.SharedLibrary;
        bEnableReflection = true;

        // Force-included so the export macros are defined in every translation unit without each source including the header.
        ForceIncludeFiles.Add("ModuleAPI.h");

        // Every engine image links the dynamic CRT; the static one arrives through some vendored
        if (Target.Platform == BuildPlatform.Windows64)
        {
            PrivateLinkerOptions.Add("/NODEFAULTLIB:LIBCMT");
        }

        // An image without its own new/delete binds to the CRT, and one out of step hands rpmalloc a CRT block.
        PerImageSourceFiles.Add(
            Path.Combine(Target.EngineSourceDirectory, "Runtime", "Source", "Memory", "GlobalAllocatorOverrides.cpp"));
    }
}

/// <summary>Defaults for third-party code the engine vendors but does not own.</summary>
public abstract class LuminaThirdPartyModuleRules : ModuleRules
{
    protected LuminaThirdPartyModuleRules(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.StaticLibrary;
        bIsThirdParty = true;
        bEnableReflection = false;
        bRootSourceFiles = true;
        bUseUnityBuild = true;
    }
}
