#pragma once

#include "Scripting/ManagedTypeRegistry.h"

#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CObject;
    class CScriptStruct;
    class CWorld;
    namespace Scripting { struct FScriptExportSchema; }
}

// Embeds CoreCLR from the bundled runtime under External/DotNet and drives the LuminaSharp managed side.
namespace Lumina::DotNet
{
    // Bumped whenever the native to managed boundary changes.
    inline constexpr int32 GAbiVersion = 17;

    // Boots the embedded runtime and runs the managed handshake.
    RUNTIME_API void Initialize();
    
    RUNTIME_API void Shutdown();

    RUNTIME_API void Tick();
    
    RUNTIME_API void ReloadScripts();

    // How many reloads have run to completion, successful or not, and the host result of the latest, where 0 is success.
    RUNTIME_API int32 GetFinishedScriptReloads();
    RUNTIME_API int32 GetLastScriptReloadResult();

    //~ Registry signal listeners, tracked per world so teardown destroys the ones script never disposed.
    RUNTIME_API void TrackSignalListener(CWorld* World, void* Listener);
    // False when the world's teardown already released the listener, so the handle must not be touched again.
    NODISCARD RUNTIME_API bool ForgetSignalListener(CWorld* World, void* Listener);

    // Drops managed state keyed on a world, early enough that its disconnects still reach a live world.
    RUNTIME_API void NotifyWorldTeardown(CWorld* World);

    // Latches a reload for the next frame start, since a reload destroys objects the frame may be using.
    RUNTIME_API void RequestScriptReload();

    // The .cs file declaring TypeName, matched on the file stem, or empty for a C++ script or a type sharing a file.
    RUNTIME_API FString FindScriptSourceFile(FStringView TypeName);

    // One script file, by the stem a script class is named after and the folder it sits in under its script root.
    struct FScriptSourceFolder
    {
        FString Stem;
        // Slash separated and relative to the script root, led by the unit's name when more than one unit has scripts.
        FString Folder;
        FString Path;
    };

    // Every script file in one pass over the script roots, which is what a picker filing scripts into folders needs.
    RUNTIME_API void GatherScriptSourceFolders(TVector<FScriptSourceFolder>& Out);

    // Latches a reload for a changed script source, held open for a quiet period since one edit arrives as several events. Thread-safe.
    RUNTIME_API void NotifyScriptSourceChanged(FStringView Path);

    // Services a latched reload once its quiet period has passed, from the frame start and never inside a draw.
    RUNTIME_API void ProcessPendingScriptReload();

    // Loads the prebuilt script DLLs a cooked game ships under DotNet/Scripts instead of compiling, a no-op without a manifest.
    RUNTIME_API void LoadCookedScripts();

    RUNTIME_API void GenerateScriptProjects();

    // One script unit the packager should ship as a prebuilt assembly in a cooked game.
    struct FPackagedScriptUnit
    {
        FString          Name;           // assembly / unit name
        FString          DllSourcePath;  // the freshly-emitted DLL on disk (may be absent if the unit had no .cs)
        TVector<FString> Deps;           // sibling units this one references (drives managed load order)
        TVector<FString> References;     // absolute third-party assemblies to stage beside the unit DLL
    };

    // Recompiles optimized and returns the unit graph the packager stages, restoring a debuggable build after. Editor only.
    RUNTIME_API void GatherScriptUnitsForPackaging(TVector<FPackagedScriptUnit>& Out);

    // Disk paths of every .cs the script units compile, listed without reading them.
    RUNTIME_API void GatherScriptSourceFiles(TVector<FString>& OutPaths);

    RUNTIME_API bool IsInitialized();

    // A managed export by name, or null. A script assembly's export dies with its generation, so re-resolve when the generation changes. Game thread only.
    RUNTIME_API void* ResolveManagedExport(FStringView Name);

    // A LuminaSharp export declared beside its caller and resolved on first call, which holds since that assembly never unloads.
    template<typename TSignature>
    class TManagedExport
    {
    public:

        explicit TManagedExport(const char* InName)
            : Name(InName)
        {}

        /** Null until the host is up, and retried until it resolves, so declaration order does not matter. */
        TSignature Get() const
        {
            if (Pointer == nullptr)
            {
                Pointer = reinterpret_cast<TSignature>(ResolveManagedExport(Name));
            }
            return Pointer;
        }

        explicit operator bool() const { return Get() != nullptr; }

        const char* GetName() const { return Name; }

    private:

        const char*        Name;
        mutable TSignature Pointer = nullptr;
    };

    //~ C# runtime diagnostics
    struct FScriptDiagnostics
    {
        int64  ManagedHeapBytes = 0;       // GC.GetTotalMemory(false)
        int64  HeapSizeBytes = 0;          // GCMemoryInfo.HeapSizeBytes
        int64  FragmentedBytes = 0;        // GCMemoryInfo.FragmentedBytes
        int64  CommittedBytes = 0;         // GCMemoryInfo.TotalCommittedBytes
        int64  TotalAllocatedBytes = 0;    // GC.GetTotalAllocatedBytes() (lifetime; drives the churn rate)
        int64  WorkingSetBytes = 0;        // Environment.WorkingSet (whole-process)
        double PauseTimePercentage = 0.0;  // GCMemoryInfo.PauseTimePercentage
        double LastPauseMs = 0.0;          // last GC pause duration

        int32  Gen0Collections = 0;
        int32  Gen1Collections = 0;
        int32  Gen2Collections = 0;
        int32  PinnedObjects = 0;          // GCMemoryInfo.PinnedObjectsCount
        int32  Generation = 0;             // current script generation
        int32  LoadedTypeCount = 0;
        int32  AliveScriptAlcCount = 0;    // collectible GameScripts.Gen* contexts still loaded (1 == healthy)
        int32  OldestAliveGeneration = 0;  // lowest still-resident generation (0 if none)
        int32  ScriptsOnline = 0;          // 1 when a generation is loaded
        int32  Reserved = 0;               // pad to an 8-byte multiple
    };
    
    RUNTIME_API bool GetRuntimeDiagnostics(FScriptDiagnostics& OutDiagnostics, bool bForceCollect = false);
    
    RUNTIME_API int32 GetScriptGeneration();
    
    //~ C# subclasses of reflected native classes, each minted as a CClass whose objects bind a managed instance.

    /** One crossing per type for everything the reload stages need, so no stage gathers for itself. */
    RUNTIME_API void GatherManagedTypeDefinitions(TVector<Scripting::FManagedTypeDefinition>& OutTypes, TVector<Scripting::FManagedTypeAlias>& OutAliases);

    // Creates the C# subclass instance for a native object, as a strong handle the object's managed-instance slot then owns.
    RUNTIME_API void* CreateScriptable(FStringView TypeName, uint64 NativePtr);

    // Runs a type's C# initializers into its defaults once at mint, every instance being copied from them.
    RUNTIME_API void ApplyScriptableDefaults(FStringView TypeName, void* DefaultObject);
}
