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
    namespace Scripting { struct FScriptExportSchema; struct FScriptPropertyEntry; struct FScriptButton; }
}

// FDotNetHost embeds CoreCLR (via the bundled runtime under External/DotNet) and
// loads the LuminaSharp managed bootstrap. Phase 1: boot + ABI handshake only.
namespace Lumina::DotNet
{
    // Bumped whenever the native<->managed boundary ABI changes.
    // v4: LoadScripts takes per-unit assembly buckets (FSourceAssembly).
    // v5: native->managed exports resolved by name (ResolveManagedExport) instead of a mirrored struct/hash.
    // v6: managed system-descriptor sink carries declared read/write component-ops tokens (parallel C# systems).
    // v7: delegate properties replace hardcoded collision/perception dispatch; adds OnNativeDelegateDestroyed.
    // v13 dropped the C# entity system bridge, since a C# system is now a CEntitySystem subclass.
    // v15 drops OnNativeDelegateDestroyed; a delegate owns its managed bindings and frees them itself.
    inline constexpr int32 GAbiVersion = 15;

    // Boots the embedded runtime and runs the managed handshake.
    RUNTIME_API void Initialize();
    
    RUNTIME_API void Shutdown();

    RUNTIME_API void Tick();
    
    RUNTIME_API void ReloadScripts();

    //~ Registry signal listeners, tracked per world so teardown destroys the ones script never disposed.
    RUNTIME_API void TrackSignalListener(CWorld* World, void* Listener);
    RUNTIME_API void ForgetSignalListener(CWorld* World, void* Listener);

    // Drops managed state keyed on a world, early enough that its disconnects still reach a live world.
    RUNTIME_API void NotifyWorldTeardown(CWorld* World);

    // Latches a reload for the next frame start, since a reload destroys objects the frame may be using.
    RUNTIME_API void RequestScriptReload();

    /** Disk path of the .cs file declaring TypeName, matched on the file stem against the script roots.
     *  Empty when nothing matches, which is the case for a C++ script or a type sharing a file. */
    RUNTIME_API FString FindScriptSourceFile(FStringView TypeName);

    /**
     * Reports that a file under a watched source tree changed, and latches a reload if it was one of ours.
     *
     * Owns both halves of the policy a caller would otherwise hard-code: which extensions are script sources,
     * and how long to wait for the burst to end. A single edit reaches an editor as several inotify events,
     * and a tool that writes a tree produces a great many, so the request is held open for a quiet period
     * rather than fired per event. Thread-safe: watcher threads call this directly.
     */
    RUNTIME_API void NotifyScriptSourceChanged(FStringView Path);

    // Services a latched request once its quiet period has elapsed. Called from the engine's frame start,
    // never from inside a draw.
    RUNTIME_API void ProcessPendingScriptReload();

    // Cooked-game variant of ReloadScripts: loads the prebuilt script DLLs the packager staged under
    // <exeDir>/DotNet/Scripts/ (driven by scripts.manifest.json) instead of compiling .cs from disk. No
    // Roslyn, no .csproj generation. Safe to call when no manifest exists (no-op).
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

    // Recompiles scripts (so every unit's DLL is freshly emitted) and returns the unit graph for the packager
    // to stage under DotNet/Scripts/ + scripts.manifest.json. Editor-side; no-op if scripting is disabled.
    RUNTIME_API void GatherScriptUnitsForPackaging(TVector<FPackagedScriptUnit>& Out);

    RUNTIME_API bool IsInitialized();

    // Resolves a native->managed export by name to its raw function pointer, or nullptr if unknown / scripting
    // is disabled. Engine exports are stable for the process; script/plugin exports (a C# [ManagedExport] in a
    // plugin's scripts) change per generation, so a caller that holds one must re-resolve when
    // GetScriptGeneration() changes (its pointer dangles once the old generation unloads). Game thread only.
    RUNTIME_API void* ResolveManagedExport(FStringView Name);

    /**
     * One managed engine export, named where it is used and resolved on first call.
     *
     * The alternative -- and what the host used to do for every export -- is a field in one central struct
     * plus a typedef plus a line in a bootstrap resolve block, so a subsystem could not add a managed entry
     * point without editing the host. Declaring the export beside its caller costs one line and nothing in
     * the host at all.
     *
     * Only for exports in LuminaSharp.dll, which is loaded for the life of the process, so a resolved pointer
     * stays valid across script reloads. A SCRIPT assembly's export dies with its generation and must be
     * resolved per use instead of held here.
     */
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
        int32  EntityScriptCount = 0;
        int32  EntitySystemCount = 0;
        int32  LoadedTypeCount = 0;
        int32  AliveScriptAlcCount = 0;    // collectible GameScripts.Gen* contexts still loaded (1 == healthy)
        int32  OldestAliveGeneration = 0;  // lowest still-resident generation (0 if none)
        int32  ScriptsOnline = 0;          // 1 when a generation is loaded
        int32  Reserved = 0;               // pad to an 8-byte multiple
    };
    
    RUNTIME_API bool GetRuntimeDiagnostics(FScriptDiagnostics& OutDiagnostics, bool bForceCollect = false);
    
    RUNTIME_API int32 GetScriptGeneration();
    
    // Entity-script lifecycle and ticking are NOT here: a C# script is a CEntityScript CObject dispatched
    // through the Reflector's ScriptEvent virtuals, so the managed side no longer owns any of it.

    RUNTIME_API void GatherEntityScriptTypes(TVector<FString>& OutTypeNames);

    //~ Scriptable CObjects: a C# subclass of a REFLECT(Scriptable) native CObject. The Reflector mints a CClass
    //  per discovered C# subclass; native creates the CObject (a minted CClass) and binds the managed instance.

    // One discovered C# Scriptable subclass: its full type name + the native base class it derives from (the
    // [ScriptableType] wrapper's reflected name). The host mints a CClass(super = that native base) per entry.
    struct FScriptableTypeDesc
    {
        FString TypeName;
        FString NativeBaseName;
        // Which ScriptEvents the C# subclass overrides. Type-uniform, so it is carried on the minted CClass
        // rather than per instance (CClass::ScriptOverrides); bit i == the wrapper's [ScriptEvent(i)].
        /** Names of the ScriptEvents this type overrides. A list rather than a mask: an override is found
         *  by name like any other function, so there is no index to agree on and no ceiling to hit. */
        TVector<FString> OverriddenEvents;
        // EScriptUpdatePhase from the class's [UpdatePhase]; type-uniform, so it rides on the minted CClass.
        uint8   UpdatePhase = 0;
    };

    // One `[Alias]` on a C# script class: the name it used to have, and the name it has now.
    struct FScriptableAlias
    {
        FString OldName;
        FString NewName;
    };

    // Reports every loaded C# Scriptable subclass + its native base. Drives runtime CClass minting + editor picker.
    RUNTIME_API void GatherScriptableTypes(TVector<FScriptableTypeDesc>& Out);

    // Reports every prior->current script class name pair, so the host can record where a renamed class went.
    RUNTIME_API void GatherScriptableAliases(TVector<FScriptableAlias>& Out);

    //~ Script data structs: a C# type marked [DataTableRow] or any other ScriptStructBase marker.
    //  Discovered on its own pass whether or not anything references it, and minted as a CScriptStruct whose
    //  super is the named native struct, so it is accepted anywhere that base is.

    // One discovered C# data type: its StableId (the simple type name, the identity an asset stores) plus the
    // native struct it derives from.
    struct FScriptStructTypeDesc
    {
        FString ScriptTypeName;
        FString NativeBaseName;
    };

    // Reports every loaded C# data type + its native base. Drives CScriptStruct minting + the editor pickers.
    /** One crossing per type for everything the reload stages need, so no stage gathers for itself. */
    RUNTIME_API void GatherManagedTypeDefinitions(TVector<Scripting::FManagedTypeDefinition>& Out);

    RUNTIME_API void GatherScriptStructTypes(TVector<FScriptStructTypeDesc>& Out);

    // Reads one data type's member schema, addressed by StableId. False when the type is unknown.
    RUNTIME_API bool GatherScriptStructSchema(FStringView ScriptTypeName, Scripting::FScriptExportSchema& OutSchema,
        TVector<Scripting::FScriptPropertyEntry>& OutDefaults);

    // Instantiates the named C# Scriptable subclass and pairs it to an already-created native object
    // (NativePtr). Returns a strong GCHandle, or nullptr on failure. The caller stores it in the object.s
    // managed-instance slot, which owns it from then on -- there is no matching Destroy: ~CObjectBase frees
    // the slot, and the teardown contract drains the whole table before the ALC unloads.
    RUNTIME_API void* CreateScriptable(FStringView TypeName, uint64 NativePtr);

    /** Runs a script type's declared [Property] initializers into its class default object. Once per type at
     *  mint, after the CDO exists; every instance is then copied from it. */
    RUNTIME_API void ApplyScriptableDefaults(FStringView TypeName, void* DefaultObject);

    //~ Exported [Property] schema bridge (editor inspector + serialization). Game thread only.

    // Builds the [Property] schema + default values for a C# script type; false if the type isn't loaded.
    RUNTIME_API bool GatherScriptSchema(FStringView ScriptClass, Scripting::FScriptExportSchema& OutSchema, TVector<Scripting::FScriptPropertyEntry>& OutDefaults);

    // Gathers the [Button] methods exposed on a C# script type (via managed reflection). Empty if the type
    // isn't loaded or declares no buttons.
    RUNTIME_API void GatherScriptButtons(FStringView ScriptClass, TVector<Scripting::FScriptButton>& OutButtons);

    // Invokes a parameterless [Button] method by name on a live script Instance. Returns false if scripting
    // is down or Instance is null. The method runs synchronously on the calling (game) thread.
    RUNTIME_API bool InvokeScriptButton(void* Instance, FStringView Method);

    // Applies per-instance values onto a live script Instance; schema-drift fields are skipped.
}
