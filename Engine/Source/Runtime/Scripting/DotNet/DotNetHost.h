#pragma once

#include "Containers/Array.h"
#include "Containers/String.h"
#include "Platform/GenericPlatform.h"
#include "Core/UpdateStage.h"

namespace Lumina
{
    struct FSystemContext;
    class CScriptStruct;
    enum class EUpdateStage : uint8;
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
    inline constexpr int32 GAbiVersion = 7;

    // Boots the embedded runtime and runs the managed handshake.
    RUNTIME_API void Initialize();
    
    RUNTIME_API void Shutdown();

    RUNTIME_API void Tick();
    
    RUNTIME_API void ReloadScripts();
    
    RUNTIME_API void GenerateScriptProjects();

    RUNTIME_API bool IsInitialized();

    // Resolves a native->managed export by name to its raw function pointer, or nullptr if unknown / scripting
    // is disabled. Engine exports are stable for the process; script/plugin exports (a C# [ManagedExport] in a
    // plugin's scripts) change per generation, so a caller that holds one must re-resolve when
    // GetScriptGeneration() changes (its pointer dangles once the old generation unloads). Game thread only.
    RUNTIME_API void* ResolveManagedExport(FStringView Name);

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
    
    RUNTIME_API void* CreateEntityScript(FStringView TypeName, uint64 World, uint32 Entity);

    RUNTIME_API void OnReadyScript(void* Instance);
    
    RUNTIME_API void UpdateScripts(void* const* Instances, int32 Count, float DeltaSeconds);

    // Dispatches OnFixedUpdate to a batch of scripts at the fixed physics step (called 0..N times per frame by
    // the SCSharpScriptSystem accumulator). No-op if the managed export is absent (old generation).
    RUNTIME_API void FixedUpdateScripts(void* const* Instances, int32 Count, float FixedDeltaSeconds);

    RUNTIME_API void DestroyEntityScript(void* Instance);
    
    RUNTIME_API void GatherEntityScriptTypes(TVector<FString>& OutTypeNames);
    
    struct FManagedSystemDesc
    {
        FString         TypeName;
        EUpdateStage    Stage = EUpdateStage::PrePhysics;
        int32           Priority = 128;
        TVector<uint32> Writes;   // entt::type_hash ids of written components (empty => exclusive system)
        TVector<uint32> Reads;    // entt::type_hash ids of read components
    };

    RUNTIME_API void GatherManagedSystemDescs(TVector<FManagedSystemDesc>& Out);


    RUNTIME_API void* CreateManagedSystem(FStringView TypeName, uint64 World);

    RUNTIME_API void DestroyManagedSystem(void* Handle);
    
    RUNTIME_API void TickManagedSystem(void* Handle, const FSystemContext* Context);

    // Bitmask of the frame callbacks a script overrides; 0 if none. Lets the caller skip the managed crossing.
    RUNTIME_API int32 GetScriptCallbackFlags(void* Instance);
    
    RUNTIME_API void DispatchScriptInput(void* Instance, int32 Type, int32 KeyCode, int32 bMouse, int32 Mods,
int32 bRepeat, double MouseX, double MouseY, double DeltaX, double DeltaY, double Scroll);

    //~ Exported [Property] schema bridge (editor inspector + serialization). Game thread only.

    // Builds the [Property] schema + default values for a C# script type; false if the type isn't loaded.
    RUNTIME_API bool GatherScriptSchema(FStringView ScriptClass, Scripting::FScriptExportSchema& OutSchema, TVector<Scripting::FScriptPropertyEntry>& OutDefaults);

    // The minted reflection layout for a C# script type, cached per script generation; null if not loaded.
    RUNTIME_API const CScriptStruct* GetScriptStruct(FStringView ScriptClass);

    // Resolves a script reference to its current full type name; empty if it resolves to no live type.
    RUNTIME_API FString ResolveScriptClassName(FStringView ScriptClass);

    // Gathers the [Button] methods exposed on a C# script type (via managed reflection). Empty if the type
    // isn't loaded or declares no buttons.
    RUNTIME_API void GatherScriptButtons(FStringView ScriptClass, TVector<Scripting::FScriptButton>& OutButtons);

    // Invokes a parameterless [Button] method by name on a live script Instance. Returns false if scripting
    // is down or Instance is null. The method runs synchronously on the calling (game) thread.
    RUNTIME_API bool InvokeScriptButton(void* Instance, FStringView Method);

    // Applies per-instance values onto a live script Instance; schema-drift fields are skipped.
    RUNTIME_API void ApplyScriptProperties(void* Instance, const TVector<Scripting::FScriptPropertyEntry>& Values);
}
