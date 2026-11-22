#include "RuntimePCH.h"
#include "World/ECS/Registry.h"
#include "EntityScript.h"

#include "Assets/AssetTypes/Mesh/MeshBuildBatch.h"
#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ManagedInstance.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Reflection/Type/Function.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/ComponentRefProperty.h"
#include "World/Entity/Components/Component.h"
#include "World/Entity/Components/ComponentRef.h"
#include "Networking/INetworkRuntime.h"
#include "Networking/NetRealm.h"
#include "UI/UIScript.h"
#include "World/WorldContext.h"
#include "Core/Engine/GameLibrary.h"
#include "World/World.h"
#include "World/WorldManager.h"
#include "ScriptableObject.h"
#include "Core/Object/ScriptClass.h"
#include "DotNet/DotNetHost.h"
#include "ScriptStruct.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/ObjectArchiver.h"
#include "World/Entity/Components/EntityTags.h"
#if USING(WITH_EDITOR)
#include "World/Entity/Components/EditorComponent.h"
#endif
#include "Log/Log.h"
#include "TaskSystem/TaskSystem.h"

namespace Lumina
{
    namespace
    {
        // Cloned, never shared, since one object on two entities carries the first's state into the second.
        CEntityScript* CloneScript(CEntityScript* Source)
        {
            if (Source == nullptr || Source->GetClass() == nullptr)
            {
                return nullptr;
            }

            CObject* Created = NewObject(Source->GetClass(), nullptr, NAME_None, FGuid::New(), OF_Transient);
            CEntityScript* Clone = static_cast<CEntityScript*>(Created);
            if (Clone != nullptr)
            {
                Source->CopyPropertiesTo(Clone);
            }
            return Clone;
        }

        void CloneScripts(const TVector<TStrongObjectPtr<CEntityScript>>& Source, TVector<TStrongObjectPtr<CEntityScript>>& Out)
        {
            Out.Reset();
            Out.Reserve(Source.Num());
            for (const TStrongObjectPtr<CEntityScript>& Held : Source)
            {
                if (CEntityScript* Clone = CloneScript(Held.Get()))
                {
                    Out.Add(Clone);
                }
            }
        }
    }

    SEntityScriptComponent::SEntityScriptComponent(const SEntityScriptComponent& Other)
    {
        CloneScripts(Other.Scripts, Scripts);
        Pending = Other.Pending;
    }

    // A component dies with its entity, which is how a script destroyed by another script's update is noticed.
    SEntityScriptComponent::~SEntityScriptComponent()
    {
        if (!Scripts.IsEmpty())
        {
            EntityScripts::NoteStructureChange();
        }
    }

    SEntityScriptComponent& SEntityScriptComponent::operator=(const SEntityScriptComponent& Other)
    {
        if (this != &Other)
        {
            CloneScripts(Other.Scripts, Scripts);
            Pending = Other.Pending;
            EntityScripts::NoteStructureChange();
        }
        return *this;
    }

    SEntityScriptComponent& SEntityScriptComponent::operator=(SEntityScriptComponent&& Other) noexcept
    {
        if (this != &Other)
        {
            Scripts = Move(Other.Scripts);
            Pending = Move(Other.Pending);
            EntityScripts::NoteStructureChange();
        }
        return *this;
    }

    namespace
    {
        // Read by the managed batch between scripts, so it is a plain 32-bit counter any thread may bump.
        std::atomic<uint32> GStructureEpoch{0};
        static_assert(sizeof(std::atomic<uint32>) == sizeof(uint32) && std::atomic<uint32>::is_always_lock_free);

        void BumpStructureEpoch()
        {
            GStructureEpoch.fetch_add(1u, std::memory_order_relaxed);
        }

#if !defined(LE_SHIPPING)
        // Owned by the registry being ticked, so each world reports a class once.
        struct FParallelPromiseReports
        {
            FMutex                  Mutex;
            THashSet<const CClass*> Classes;
        };

        // The script running in a parallel chunk is Scripts[*Cursor], the cursor being one a managed batch advances, or 0 when null.
        struct FParallelScriptRun
        {
            FParallelPromiseReports* Reports = nullptr;
            CEntityScript* const*    Scripts = nullptr;
            const int32*             Cursor  = nullptr;
        };

        thread_local FParallelScriptRun* GParallelScriptRun = nullptr;

        // Returns the running script whose class has not been reported yet, or null.
        const CEntityScript* FirstBrokenPromise(ECS::FEntity Touched)
        {
            const FParallelScriptRun* Run = GParallelScriptRun;
            if (Run == nullptr || Run->Scripts == nullptr)
            {
                return nullptr;
            }
            const CEntityScript* Script = Run->Scripts[Run->Cursor != nullptr ? *Run->Cursor : 0];
            if (!Touched.IsNull() && Touched == Script->GetOwningEntity())
            {
                return nullptr;
            }
            FScopeLock Lock(Run->Reports->Mutex);
            return Run->Reports->Classes.TryAdd(Script->GetClass()) ? Script : nullptr;
        }
#endif
    }

#if !defined(LE_SHIPPING)
    void EntityScripts::CheckParallelAccess(ECS::FEntity Entity, const char* Call)
    {
        if (const CEntityScript* Script = FirstBrokenPromise(Entity))
        {
            LOG_ERROR("ParallelUpdate script {} called {} on entity {}, which is not its own, during the parallel pass. "
                "ParallelUpdate promises a script touches only its own entity, so this is a data race.",
                Script->GetClass()->GetName(), Call, Entity.GetPacked());
        }
    }

    void EntityScripts::CheckParallelStructureChange(const char* Call)
    {
        if (const CEntityScript* Script = FirstBrokenPromise(ECS::NullEntity))
        {
            LOG_ERROR("ParallelUpdate script {} made a structural change through {} during the parallel pass. "
                "ParallelUpdate promises a script touches only its own entity, so this is a data race.",
                Script->GetClass()->GetName(), Call);
        }
    }
#endif

    // A script destroyed outright, not detached, must still stop a batch that has it queued.
    CEntityScript::~CEntityScript()
    {
        EntityScripts::NoteStructureChange();
        UIScripts::Forget(this);
        if (BindingToken != nullptr)
        {
            BindingToken->End();
            BindingToken->Release();
        }
    }

    FBindingToken* FEntityScriptAccess::BindingToken(CEntityScript& Script)
    {
        if (Script.BindingToken == nullptr)
        {
            Script.BindingToken = FBindingToken::Create();
            if (Script.bBindingsEnded)
            {
                Script.BindingToken->End();
            }
        }
        return Script.BindingToken;
    }

    void FEntityScriptAccess::EndBindings(CEntityScript& Script)
    {
        Script.bBindingsEnded = true;
        if (Script.BindingToken != nullptr)
        {
            Script.BindingToken->End();
        }
    }

    // A fault is not a broken promise, so it stops a parallel pass without being reported as one.
    void CEntityScript::OnReinstanced(CObject& Previous)
    {
        CEntityScript* Original = Cast<CEntityScript>(&Previous);
        if (Original == nullptr)
        {
            return;
        }
        OwningEntity = Original->OwningEntity;
        OwningWorld  = Original->OwningWorld;
        bReady       = Original->bReady;
        bFaulted     = Original->bFaulted;
        UIScripts::Replace(Original, this);

        // The original's handlers would keep running on it, so a reload rebinds through OnReloaded instead.
        FEntityScriptAccess::EndBindings(*Original);

        // A reload can add a reference the original never had, and OnAttach does not run again to bind it.
        BindComponentRefs();
    }

    void CEntityScript::BindComponentRefs()
    {
        if (OwningWorld == nullptr || OwningEntity == ECS::NullEntity)
        {
            return;
        }

        for (FProperty* Property : GetClass()->GetProperties())
        {
            if (Property->GetType() != EPropertyTypeFlags::ComponentRef || !Property->HasMetadata("Component"))
            {
                continue;
            }

            const CStruct* Component = static_cast<const FComponentRefProperty*>(Property)->GetComponentStruct();
            const FComponentOps* Ops = Component != nullptr ? Component->GetComponentOps() : nullptr;
            if (Ops == nullptr)
            {
                LOG_WARN("{}.{} names {}, which is not a component, so it is left unbound.", *GetClass()->GetName(),
                    *Property->GetPropertyName(), Component != nullptr ? *Component->GetName() : "nothing");
                continue;
            }

            const bool bAdd = Property->GetMetadata("Component") != "Get";
            Property->GetValuePtr<FComponentRef>(this)->Bind(*OwningWorld, OwningEntity, *Ops, bAdd);
        }
    }

    void CEntityScript::MarkFaulted()
    {
        bFaulted = true;
        BumpStructureEpoch();
    }

    void CEntityScript::SetUpdatePriority(int32 InPriority)
    {
        if (UpdatePriority == InPriority)
        {
            return;
        }
        UpdatePriority = InPriority;

        // The claim rebuild is what sorts an entity's scripts, so an attached script's change has to trigger one.
        if (OwningEntity != ECS::NullEntity)
        {
            EntityScripts::NoteStructureChange();
        }
    }

    namespace
    {
        // Null while the class is still not loaded.
        CEntityScript* RestoreHeldScript(const SHeldValue& Held)
        {
            CClass* ScriptClass = FScriptableRegistry::ResolveClass(Held.TypeName);
            if (ScriptClass == nullptr || !ScriptClass->IsChildOf(CEntityScript::StaticClass()))
            {
                return nullptr;
            }

            CEntityScript* Script = static_cast<CEntityScript*>(
                NewObject(ScriptClass, nullptr, NAME_None, FGuid::New(), OF_Transient));
            if (Script != nullptr && !Held.Data.IsEmpty())
            {
                const TUniquePtr<FArchive> Reader = Held.Data.MakeReader();
                ScriptClass->SerializeProperties(*Reader, Script);
                Held.ApplyEntityRemap(ScriptClass, Script);
            }
            return Script;
        }
    }

    bool SEntityScriptComponent::Serialize(FArchive& Ar)
    {
        if (Ar.IsWriting())
        {
            int32 Count = 0;
            for (const TStrongObjectPtr<CEntityScript>& Held : Scripts)
            {
                if (Held.Get() != nullptr && Held.Get()->GetClass() != nullptr)
                {
                    ++Count;
                }
            }
            Ar << Count;

            for (const TStrongObjectPtr<CEntityScript>& Held : Scripts)
            {
                CEntityScript* Script = Held.Get();
                if (Script == nullptr || Script->GetClass() == nullptr)
                {
                    continue;
                }
                FName ClassName = Script->GetClass()->GetName();
                Ar << ClassName;

                // Length-prefixed, so a reader that cannot resolve this class can skip exactly this script.
                const int64 SizePos = Ar.Tell();
                int64 ScriptSize = 0;
                Ar << ScriptSize;

                const int64 DataStart = Ar.Tell();
                Script->GetClass()->SerializeProperties(Ar, Script);
                const int64 DataEnd = Ar.Tell();

                ScriptSize = DataEnd - DataStart;
                Ar.Seek(SizePos);
                Ar << ScriptSize;
                Ar.Seek(DataEnd);
            }

            // Carried with their own names and objects, so a world saved while a script failed to compile keeps it.
            Ar << Pending;
            return true;
        }

        if (Ar.IsReading())
        {
            int32 Count = 0;
            Ar << Count;

            // Older files have no per-script length, so an unresolvable class there still ends the record.
            const bool bLengthPrefixed =
                Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::ENTITY_SCRIPT_LENGTH_PREFIX;

            Scripts.Reset();
            Pending.Reset();

            // Reading in place swaps the scripts a cached claim points at, and the old ones may outlive this.
            EntityScripts::NoteStructureChange();
            for (int32 Index = 0; Index < Count; ++Index)
            {
                FName ClassName;
                Ar << ClassName;

                int64 ScriptSize = 0;
                int64 DataStart  = 0;
                if (bLengthPrefixed)
                {
                    Ar << ScriptSize;
                    DataStart = Ar.Tell();
                }

                // Resolved through the redirect registry, since an alias is what carries a renamed class across.
                CClass* ScriptClass = FScriptableRegistry::ResolveClass(ClassName);
                CEntityScript* Script = nullptr;
                if (ScriptClass != nullptr && ScriptClass->IsChildOf(CEntityScript::StaticClass()))
                {
                    Script = static_cast<CEntityScript*>(
                        NewObject(ScriptClass, nullptr, NAME_None, FGuid::New(), OF_Transient));
                }

                if (Script == nullptr)
                {
                    if (!bLengthPrefixed)
                    {
                        LOG_WARN("SEntityScriptComponent: script class '{}' could not be resolved and this "
                                 "file has no per-script length, so the rest were dropped.", *ClassName);
                        break;
                    }

                    // Held rather than discarded, since the class is usually just not loaded yet.
                    SHeldValue Held;
                    Held.TypeName = ClassName;
                    if (Held.Data.Cut(Ar, ScriptSize))
                    {
                        Pending.Add(Move(Held));
                        LOG_WARN("SEntityScriptComponent: script class '{}' is not loaded; holding it until a "
                                 "script reload can restore it.", *ClassName);
                    }
                    else
                    {
                        LOG_WARN("SEntityScriptComponent: script class '{}' is not loaded and this archive cannot "
                                 "hold it; dropped.", *ClassName);
                    }
                    Ar.Seek(DataStart + ScriptSize);
                    continue;
                }

                ScriptClass->SerializeProperties(Ar, Script);

                // Trusted over wherever the property reader stopped, so drift cannot shift the next script.
                if (bLengthPrefixed)
                {
                    Ar.Seek(DataStart + ScriptSize);
                }

                Scripts.Add(Script);
            }

            if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::PORTABLE_HELD_SCRIPTS)
            {
                TVector<SHeldValue> Held;
                Ar << Held;
                for (SHeldValue& Value : Held)
                {
                    if (CEntityScript* Script = RestoreHeldScript(Value))
                    {
                        Scripts.Add(Script);
                        continue;
                    }
                    Pending.Add(Move(Value));
                }
            }
            return true;
        }

        return true;
    }

    namespace
    {
        // Snapshotting as a strong ref keeps a script detached mid-pass alive until the snapshot dies.

        using FScriptSnapshot = TVector<TStrongObjectPtr<CEntityScript>>;

        // Runs OnUpdate for a run of C# scripts in one crossing and returns how many ran before the epoch moved.
        DotNet::TManagedExport<int32 (*)(void* const*, int32, float, const uint32*, int32*)> GDispatchUpdates("DispatchEntityScriptUpdates");

        // Whether a class's OnUpdate is a C# override, which is what the batch can run without the native shim.
        bool HasManagedUpdate(const CClass* Class)
        {
            static const FName OnUpdateName("OnUpdate");
            return Class != nullptr && FindScriptOverride(Class, OnUpdateName) != nullptr;
        }

        // Below this many scripts the pass stays on the calling thread, where a fan-out costs more than it saves.
        constexpr uint32 kParallelMinScripts   = 512;
        constexpr uint32 kParallelChunkScripts = 256;

        // Inherited, so a subclass keeps its parent's promise.
        bool IsParallelUpdate(const CClass* Class)
        {
            static const FName ParallelUpdateName("ParallelUpdate");
            for (const CClass* Current = Class; Current != nullptr; Current = Current->GetSuperClass())
            {
                if (Current->HasMeta(ParallelUpdateName))
                {
                    return true;
                }
            }
            return false;
        }

        // Cut on entity boundaries, so every script of one entity runs on one thread in its usual order.
        void BuildEntityChunks(const TVector<ECS::FEntity>& Entities, TVector<uint32>& OutStarts)
        {
            OutStarts.Reset();
            const uint32 Num = (uint32)Entities.Num();
            uint32 Start = 0;
            while (Start < Num)
            {
                OutStarts.Add(Start);
                uint32 End = Start + kParallelChunkScripts < Num ? Start + kParallelChunkScripts : Num;
                while (End < Num && Entities[End] == Entities[End - 1])
                {
                    ++End;
                }
                Start = End;
            }
            OutStarts.Add(Num);
        }

        template<typename TFunc>
        void RunChunks(uint32 NumScripts, uint32 NumChunks, TFunc&& RunChunk)
        {
            if (NumScripts < kParallelMinScripts || NumChunks <= 1)
            {
                for (uint32 Chunk = 0; Chunk < NumChunks; ++Chunk)
                {
                    RunChunk(Chunk);
                }
                return;
            }
            Jobs::ParallelFor(NumChunks, RunChunk, 1);
        }

        // Phase, opt-in and override are class data, so a run of one class resolves them once.
        struct FClassFacts
        {
            const CClass*      Class     = nullptr;
            bool               bParallel = false;
            bool               bManaged  = false;
            EScriptUpdatePhase Phase     = EScriptUpdatePhase::PrePhysics;

            void See(const CEntityScript* Script);
        };

        // Which cached claim a pass reads. The first two match EScriptUpdatePhase, and a fixed step ticks scripts of every phase.
        enum class EClaimPass : uint8
        {
            PrePhysics,
            PostPhysics,
            Fixed,
            Num,
        };

        enum class EClaimResult : uint8
        {
            Serial,
            Claimed,
            Idle,
            NeedsInstance,
        };

        struct FClaimChunk
        {
            TVector<void*>          Handles;
            TVector<CEntityScript*> Scripts;
            TVector<ECS::FEntity>   Entities;
            bool                    bNeedsInstance = false;
        };

        // Above this many entities the claim is cut into chunks of this size and spread across the workers.
        constexpr uint32 kParallelClaimMinEntities = 2048;
        constexpr uint32 kParallelClaimChunk       = 512;

        // Everything a claim was built from, so an unchanged key means the same entities would claim the same scripts.
        struct FClaimKey
        {
            uint32 StructureEpoch   = 0;
            uint32 HandleGeneration = 0;
            uint32 PoolLayouts[3]   = {};
            int32  PoolSizes[3]     = {};

            NODISCARD bool operator==(const FClaimKey&) const = default;
        };

        // One pass's claim, kept across ticks while its key holds. Raw script pointers are safe, as freeing one moves the epoch.
        struct FClaim
        {
            FClaimKey               Key;
            bool                    bComplete = false;
            bool                    bLeased   = false;

            // The scripted entities in walk order, each marked handled when it was claimed or has nothing to run this pass.
            TVector<ECS::FEntity>   Entities;
            TVector<uint8>          Handled;
            uint32                  NumSerial = 0;

            // One entry per claimed script in entity order, with a null handle for a script that is called directly.
            TVector<void*>          Handles;
            TVector<CEntityScript*> Scripts;
            TVector<ECS::FEntity>   ScriptEntities;
            TVector<FClaimChunk>    Chunks;
        };

        struct FScriptClaims
        {
            FClaim Passes[(uint8)EClaimPass::Num];
        };

        // A nested tick of the same registry claims into its own storage rather than rebuilding the claim the outer pass walks.
        class FClaimLease
        {
        public:

            FClaimLease(ECS::FRegistry& Registry, EClaimPass Pass)
            {
                FClaim& Cached = Registry.Ctx().GetOrEmplace<FScriptClaims>().Passes[(uint8)Pass];
                if (!Cached.bLeased)
                {
                    Cached.bLeased = true;
                    Leased = &Cached;
                }
            }

            ~FClaimLease()
            {
                if (Leased != nullptr)
                {
                    Leased->bLeased = false;
                }
            }

            LE_NO_COPYMOVE(FClaimLease);

            FClaim& Get() { return (Leased != nullptr) ? *Leased : Local; }

        private:

            FClaim* Leased = nullptr;
            FClaim  Local;
        };

#if defined(LE_SHIPPING)
        void PrepareParallelPass(ECS::FRegistry&) {}
        void WatchParallelRun(CEntityScript* const*, const int32*) {}

        class FParallelScriptPassScope
        {
        public:

            explicit FParallelScriptPassScope(ECS::FRegistry&) {}

            LE_NO_COPYMOVE(FParallelScriptPassScope);
        };
#else
        // On the ticking thread before the fan-out, since a worker only finds the reports.
        void PrepareParallelPass(ECS::FRegistry& Registry)
        {
            if (Registry.Ctx().Find<TUniquePtr<FParallelPromiseReports>>() == nullptr)
            {
                Registry.Ctx().Emplace<TUniquePtr<FParallelPromiseReports>>(MakeUnique<FParallelPromiseReports>());
            }
        }

        void WatchParallelRun(CEntityScript* const* Scripts, const int32* Cursor)
        {
            if (GParallelScriptRun != nullptr)
            {
                GParallelScriptRun->Scripts = Scripts;
                GParallelScriptRun->Cursor  = Cursor;
            }
        }

        class FParallelScriptPassScope
        {
        public:

            explicit FParallelScriptPassScope(ECS::FRegistry& Registry)
            {
                Run.Reports = Registry.Ctx().Find<TUniquePtr<FParallelPromiseReports>>()->Get();
                GParallelScriptRun = &Run;
            }
            ~FParallelScriptPassScope() { GParallelScriptRun = nullptr; }

            LE_NO_COPYMOVE(FParallelScriptPassScope);

        private:

            FParallelScriptRun Run;
        };
#endif

        // Whatever a parallel run did not get to goes the checked serial way, finishing an interrupted entity first.
        template<typename TRunScript, typename TRunEntity>
        void FinishRemainder(const TVector<ECS::FEntity>& Entities, const TVector<CEntityScript*>& Scripts, uint32 Begin, uint32 Ran, uint32 End,
            TRunScript&& RunScript, TRunEntity&& RunEntity)
        {
            uint32 Index = Begin + Ran;
            if (Index >= End)
            {
                return;
            }
            if (Ran > 0)
            {
                const ECS::FEntity Interrupted = Entities[Index - 1];
                for (; Index < End && Entities[Index] == Interrupted; ++Index)
                {
                    RunScript(Interrupted, Scripts[Index]);
                }
            }
            for (ECS::FEntity Previous = ECS::NullEntity; Index < End; ++Index)
            {
                if (Entities[Index] != Previous)
                {
                    Previous = Entities[Index];
                    RunEntity(Previous);
                }
            }
        }

        // RunSpan returns how many scripts of its span ran before a structural change stopped it.
        template<typename TRunSpan, typename TRunScript, typename TRunEntity>
        void RunInEntityChunks(ECS::FRegistry& Registry, TVector<uint32>& ChunkStarts, TVector<uint32>& ChunkRan, const TVector<ECS::FEntity>& Entities,
            const TVector<CEntityScript*>& Scripts, TRunSpan&& RunSpan, TRunScript&& RunScript, TRunEntity&& RunEntity)
        {
            BuildEntityChunks(Entities, ChunkStarts);
            const uint32 NumChunks = (uint32)ChunkStarts.Num() - 1;
            ChunkRan.Init(0, NumChunks);

            PrepareParallelPass(Registry);
            RunChunks((uint32)Scripts.Num(), NumChunks, [&](uint32 Chunk)
            {
                FParallelScriptPassScope PassScope(Registry);
                ChunkRan[Chunk] = RunSpan(ChunkStarts[Chunk], ChunkStarts[Chunk + 1]);
            });

            for (uint32 Chunk = 0; Chunk < NumChunks; ++Chunk)
            {
                FinishRemainder(Entities, Scripts, ChunkStarts[Chunk], ChunkRan[Chunk], ChunkStarts[Chunk + 1], RunScript, RunEntity);
            }
        }

        // Both tick passes rebuild these every call, so they are parked per thread rather than reallocated.
        struct FTickScratch
        {
            FScriptSnapshot       Scripts;

            // Unpinned, since freeing a queued script means detaching or destroying it, and either one moves the epoch.
            TVector<void*>          BatchHandles;
            TVector<ECS::FEntity>   BatchEntities;
            TVector<CEntityScript*> BatchScripts;

            TVector<uint32>         ChunkStarts;
            TVector<uint32>         ChunkRan;
        };

        // A nested tick falls back to its own storage rather than walking the outer pass's buffers.
        class FTickScratchGuard
        {
        public:

            FTickScratchGuard()
            {
                if (Depth()++ == 0u)
                {
                    Parked = &ParkedStorage();
                }
            }

            ~FTickScratchGuard() { --Depth(); }

            LE_NO_COPYMOVE(FTickScratchGuard);

            FTickScratch& Get() { return (Parked != nullptr) ? *Parked : Local; }

        private:

            static uint32& Depth()               { static thread_local uint32 Value = 0; return Value; }
            static FTickScratch& ParkedStorage() { static thread_local FTickScratch Storage; return Storage; }

            FTickScratch* Parked = nullptr;
            FTickScratch  Local;
        };

        void SnapshotScripts(ECS::FRegistry& Registry, ECS::FEntity Entity, FScriptSnapshot& Out)
        {
            Out.Reset();

            // try_get on a dead entity indexes the sparse set out of bounds rather than returning null.
            if (Entity == ECS::NullEntity || !Registry.IsValid(Entity))
            {
                return;
            }

            SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity);
            if (Component == nullptr)
            {
                return;
            }

            Out.Reserve(Component->Scripts.Num());
            for (const TStrongObjectPtr<CEntityScript>& Held : Component->Scripts)
            {
                if (Held.Get() != nullptr)
                {
                    Out.Add(Held);
                }
            }
        }

        // Type-uniform, so it is one class-level byte rather than anything stored per script instance.
        EScriptUpdatePhase ScriptPhase(const CEntityScript* Script)
        {
            const CScriptClass* Class = Script != nullptr ? ToScriptClass(Script->GetClass()) : nullptr;
            return Class != nullptr ? static_cast<EScriptUpdatePhase>(Class->ScriptUpdatePhase)
                                    : EScriptUpdatePhase::PrePhysics;
        }

        bool IsOutsideRealm(const CEntityScript* Script, CWorld* World)
        {
            return !NetRealm::Allows(Script->GetClass(), World);
        }

        // Re-resolved per dispatch, since an earlier callback may have removed this script or its entity.
        bool IsStillAttached(ECS::FRegistry& Registry, ECS::FEntity Entity, const CEntityScript* Script)
        {
            if (Script == nullptr || !Registry.IsValid(Entity))
            {
                return false;
            }

            const SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity);
            if (Component == nullptr)
            {
                return false;
            }

            for (const TStrongObjectPtr<CEntityScript>& Held : Component->Scripts)
            {
                if (Held.Get() == Script)
                {
                    return true;
                }
            }
            return false;
        }

        // A loaded prefab owns a registry without being a world, so its scripts need sweeping too. A script attached mid-pass readies next tick.
        void SnapshotScriptedEntities(ECS::FRegistry& Registry, TVector<ECS::FEntity>& Out)
        {
            Out.Reset();

            // With nothing excluded and no holes the view is the pool itself, copied back to front in its walk order.
            const auto Pool     = Registry.FindStorage<SEntityScriptComponent>();
            const auto Disabled = Registry.FindStorage<SDisabledTag>();
            const auto Paused   = Registry.FindStorage<SScriptDisabledTag>();
            if (Pool && !Pool->HasTombstones() && (!Disabled || Disabled->IsEmpty()) && (!Paused || Paused->IsEmpty()))
            {
                const ECS::FEntity* Dense = Pool->GetDenseData();
                const size_t        Num   = Pool->GetDenseSize();
                Out.SetNum(IntCastChecked<int32>(Num));
                for (int32 Index = 0, Limit = IntCastChecked<int32>(Num); Index < Limit; ++Index)
                {
                    Out[Index] = Dense[Num - 1u - Index];
                }
                return;
            }

            auto View = Registry.View<SEntityScriptComponent, ECS::Without<SDisabledTag>, ECS::Without<SScriptDisabledTag>>();
            Out.Reserve(View.Num());
            for (ECS::FEntity Entity : View)
            {
                Out.Add(Entity);
            }
        }

        // Owner, managed instance and component references, then OnAttach. False for a script this world's realm leaves out.
        bool AttachScript(CEntityScript* Script, ECS::FEntity Entity, CWorld* World)
        {
            if (IsOutsideRealm(Script, World))
            {
                return false;
            }
            FEntityScriptAccess::SetOwner(*Script, Entity, World);

            // A loaded C# script with no callbacks would otherwise never get its instance, and lookups would wrap it as the base class.
            if (Cast<CScriptClass>(Script->GetClass()) != nullptr)
            {
                Scriptable::GetOrCreateInstance(Script);
            }

            LUMINA_PROFILE_SECTION_NAMED(*Script->GetClass()->GetName());
            FEntityScriptAccess::BindComponentRefs(*Script);
            Script->OnAttach();
            return true;
        }

        void ReadyScript(CEntityScript* Script)
        {
            FEntityScriptAccess::MarkReady(*Script);

            // Readiness decides what ticks, so a cached claim has to see it.
            BumpStructureEpoch();

            // Once per script, so naming it by class costs nothing per frame and splits the script system's time.
            LUMINA_PROFILE_SECTION_NAMED(*Script->GetClass()->GetName());

            // Before OnReady, so a UI script's document, bound values and elements are already there.
            UIScripts::Open(Script);

            // Bound again, since a Get reference made at attach misses a component added before this first tick.
            FEntityScriptAccess::BindComponentRefs(*Script);
            Script->OnReady();
        }

        NODISCARD bool NeedsAttach(const CEntityScript* Script)
        {
            return Script != nullptr && Script->GetOwningEntity() == ECS::NullEntity;
        }

        NODISCARD bool NeedsReady(const CEntityScript* Script)
        {
            return Script != nullptr && Script->GetOwningEntity() != ECS::NullEntity && !FEntityScriptAccess::IsReady(*Script) && !FEntityScriptAccess::IsFaulted(*Script);
        }

        // Where an entity sits in the scene tree, as a sort key. An entity outside any hierarchy is a tree of its own.
        struct FTreePosition
        {
            ECS::FEntity Entity;
            uint32       Root       = 0;
            uint32       Slot       = 0;
            uint32       SubtreeEnd = 1;

            NODISCARD bool Contains(const FTreePosition& Other) const
            {
                return Root == Other.Root && Slot < Other.Slot && Other.Slot < SubtreeEnd;
            }
        };

        // Gathers the entities owing a lifecycle callback, placed in the tree so they can be sorted.
        template <typename TPredicate>
        void GatherPending(ECS::FRegistry& Registry, TVector<ECS::FEntity>& Scratch, TVector<FTreePosition>& Out, TPredicate&& IsPending)
        {
            Out.Reset();
            SnapshotScriptedEntities(Registry, Scratch);

            const ECS::FHierarchy& Hierarchy = Registry.GetHierarchy();
            bool bOrdered = false;
            for (const ECS::FEntity Entity : Scratch)
            {
                const SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity);
                if (Component == nullptr)
                {
                    continue;
                }

                bool bPending = false;
                for (const TStrongObjectPtr<CEntityScript>& Held : Component->Scripts)
                {
                    bPending = bPending || IsPending(Held.Get());
                }
                if (!bPending)
                {
                    continue;
                }

                if (!bOrdered)
                {
                    Hierarchy.EnsureOrder();
                    bOrdered = true;
                }

                FTreePosition& Position = Out.EmplaceGetRef();
                Position.Entity = Entity;
                Position.Root   = Entity.GetIndex();
                const uint32 Slot = Hierarchy.GetSlot(Entity);
                if (Slot != ECS::FHierarchy::NoSlot)
                {
                    Position.Root       = Hierarchy.GetRoot(Entity).GetIndex();
                    Position.Slot       = Slot;
                    Position.SubtreeEnd = Slot + Hierarchy.GetSubtreeSizes()[Slot];
                }
            }
        }

        // Godot's order. Parents attach before their children, children ready before their parents, and every attach precedes every ready.
        void DrainLifecycle(ECS::FRegistry& Registry, CWorld* World)
        {
            TVector<ECS::FEntity>  Scratch;
            TVector<FTreePosition> Pending;
            FScriptSnapshot        Scripts;

            // A callback that spawns scripted entities queues another round, so they join this frame in the same order.
            constexpr int32 kMaxRounds = 8;
            int32 Round = 0;
            for (; Round < kMaxRounds; ++Round)
            {
                bool bAnyWork = false;

                GatherPending(Registry, Scratch, Pending, NeedsAttach);
                Algo::Sort(Pending.begin(), Pending.end(), [](const FTreePosition& A, const FTreePosition& B)
                {
                    return A.Root != B.Root ? A.Root < B.Root : A.Slot < B.Slot;
                });
                for (const FTreePosition& Position : Pending)
                {
                    SnapshotScripts(Registry, Position.Entity, Scripts);
                    for (TStrongObjectPtr<CEntityScript>& Held : Scripts)
                    {
                        CEntityScript* Script = Held.Get();
                        if (NeedsAttach(Script) && IsStillAttached(Registry, Position.Entity, Script))
                        {
                            AttachScript(Script, Position.Entity, World);
                            bAnyWork = true;
                        }
                    }
                }

                GatherPending(Registry, Scratch, Pending, NeedsReady);
                Algo::Sort(Pending.begin(), Pending.end(), [](const FTreePosition& A, const FTreePosition& B)
                {
                    if (A.Root != B.Root)
                    {
                        return A.Root < B.Root;
                    }
                    if (A.Contains(B))
                    {
                        return false;
                    }
                    return B.Contains(A) || A.Slot < B.Slot;
                });
                for (const FTreePosition& Position : Pending)
                {
                    SnapshotScripts(Registry, Position.Entity, Scripts);
                    for (TStrongObjectPtr<CEntityScript>& Held : Scripts)
                    {
                        CEntityScript* Script = Held.Get();
                        if (NeedsReady(Script) && IsStillAttached(Registry, Position.Entity, Script))
                        {
                            ReadyScript(Script);
                            bAnyWork = true;
                        }
                    }
                }

                if (!bAnyWork)
                {
                    break;
                }
            }

            if (Round == kMaxRounds)
            {
                GatherPending(Registry, Scratch, Pending, [](const CEntityScript* Script) { return NeedsAttach(Script) || NeedsReady(Script); });
                if (!Pending.IsEmpty())
                {
                    LOG_WARN_ONCE("Entity scripts spawned past the {} lifecycle rounds a frame allows, so the {} still pending run OnAttach or OnReady out of tree order or a frame late",
                                  kMaxRounds, Pending.Num());
                }
            }
            Scripts.Reset();
        }

        void FClassFacts::See(const CEntityScript* Script)
        {
            if (Script->GetClass() != Class)
            {
                Class     = Script->GetClass();
                bParallel = IsParallelUpdate(Class);
                bManaged  = HasManagedUpdate(Class);
                Phase     = ScriptPhase(Script);
            }
        }

        NODISCARD bool TicksIn(const CEntityScript* Script, const FClassFacts& Facts, EClaimPass Pass)
        {
            return FEntityScriptAccess::ShouldTick(*Script) && (Pass == EClaimPass::Fixed || Facts.Phase == static_cast<EScriptUpdatePhase>(Pass));
        }

        // Queues the entity's ticking scripts when all of them promised to touch nothing else and owe no lifecycle.
        EClaimResult ClaimEntity(ECS::FRegistry& Registry, ECS::FEntity Entity, EClaimPass Pass, bool bMayCreateInstances, FClassFacts& Facts,
                                 TVector<void*>& OutHandles, TVector<CEntityScript*>& OutScripts, TVector<ECS::FEntity>& OutEntities)
        {
            SEntityScriptComponent* Component = (Entity != ECS::NullEntity && Registry.IsValid(Entity))
                ? Registry.TryGet<SEntityScriptComponent>(Entity) : nullptr;
            if (Component == nullptr)
            {
                return EClaimResult::Idle;
            }

            // Every priority change moves the epoch, so sorting here orders the claimed span and every later walk of the list.
            Algo::StableSort(Component->Scripts, [](const TStrongObjectPtr<CEntityScript>& A, const TStrongObjectPtr<CEntityScript>& B)
            {
                return (A.Get() != nullptr ? A->GetUpdatePriority() : 0) < (B.Get() != nullptr ? B->GetUpdatePriority() : 0);
            });

            // Attach and OnReady are owed whatever the script's phase, and only the PrePhysics pass runs them.
            const bool bDrainLifecycle = Pass == EClaimPass::PrePhysics;
            bool bAnyTicking = false;
            for (const TStrongObjectPtr<CEntityScript>& Held : Component->Scripts)
            {
                const CEntityScript* Script = Held.Get();
                if (Script == nullptr || (bDrainLifecycle && (Script->GetOwningEntity() == ECS::NullEntity || (!FEntityScriptAccess::IsFaulted(*Script) && !FEntityScriptAccess::IsReady(*Script)))))
                {
                    return EClaimResult::Serial;
                }
                Facts.See(Script);
                if (!TicksIn(Script, Facts, Pass))
                {
                    continue;
                }
                if (!Facts.bParallel)
                {
                    return EClaimResult::Serial;
                }
                bAnyTicking = true;
            }
            if (!bAnyTicking)
            {
                return EClaimResult::Idle;
            }

            const int32 Start = OutScripts.Num();
            for (const TStrongObjectPtr<CEntityScript>& Held : Component->Scripts)
            {
                CEntityScript* Script = Held.Get();
                Facts.See(Script);
                if (!TicksIn(Script, Facts, Pass))
                {
                    continue;
                }

                // A fixed step crosses per script, so it never batches through a managed handle.
                void* Handle = nullptr;
                if (Facts.bManaged && Pass != EClaimPass::Fixed)
                {
                    const uint32 Generation = ManagedInstances::GetHandleGeneration();
                    Handle = FEntityScriptAccess::CachedHandleGeneration(*Script) == Generation ? FEntityScriptAccess::CachedManagedHandle(*Script) : nullptr;
                    if (Handle == nullptr)
                    {
                        // Creating one runs a managed constructor, which only the serial pass may do.
                        if (!bMayCreateInstances)
                        {
                            OutHandles.SetNum(Start);
                            OutScripts.SetNum(Start);
                            OutEntities.SetNum(Start);
                            return EClaimResult::NeedsInstance;
                        }
                        Handle = Scriptable::GetOrCreateInstance(Script);
                        FEntityScriptAccess::CachedManagedHandle(*Script)    = Handle;
                        FEntityScriptAccess::CachedHandleGeneration(*Script) = Generation;
                    }
                    if (Handle == nullptr)
                    {
                        OutHandles.SetNum(Start);
                        OutScripts.SetNum(Start);
                        OutEntities.SetNum(Start);
                        return EClaimResult::Serial;
                    }
                }
                OutHandles.Add(Handle);
                OutScripts.Add(Script);
                OutEntities.Add(Entity);
            }
            return EClaimResult::Claimed;
        }

        // Runs claimed scripts in order until the structure epoch moves, returning how many ran.
        uint32 RunClaimedSpan(void* const* Handles, CEntityScript* const* Scripts, uint32 Count, float DeltaTime, uint32 ClaimEpoch)
        {
            auto* Dispatch = GDispatchUpdates.Get();
            const uint32* EpochAddress = reinterpret_cast<const uint32*>(&GStructureEpoch);

            uint32 Index = 0;
            int32 Cursor = 0;
            while (Index < Count && GStructureEpoch.load(std::memory_order_relaxed) == ClaimEpoch)
            {
                if (Handles[Index] == nullptr)
                {
                    CEntityScript* Script = Scripts[Index];
                    LUMINA_PROFILE_SECTION_NAMED(*Script->GetClass()->GetName());
                    WatchParallelRun(Scripts + Index, nullptr);
                    Script->OnUpdate(DeltaTime);
                    ++Index;
                    continue;
                }
                if (Dispatch == nullptr)
                {
                    break;
                }

                uint32 RunEnd = Index;
                while (RunEnd < Count && Handles[RunEnd] != nullptr)
                {
                    ++RunEnd;
                }
                LUMINA_PROFILE_SECTION_NAMED(*Scripts[Index]->GetClass()->GetName());
                WatchParallelRun(Scripts + Index, &Cursor);
                const uint32 Ran = (uint32)Dispatch(const_cast<void**>(Handles) + Index, (int32)(RunEnd - Index), DeltaTime, EpochAddress, &Cursor);
                Index += Ran;
                if (Index < RunEnd)
                {
                    break;
                }
            }
            return Index;
        }

        // A pool's membership cannot change without its live count or its layout epoch moving, since every removal bumps the layout.
        FClaimKey MakeClaimKey(const ECS::FRegistry& Registry)
        {
            FClaimKey Key;
            Key.StructureEpoch   = GStructureEpoch.load(std::memory_order_relaxed);
            Key.HandleGeneration = ManagedInstances::GetHandleGeneration();

            const ECS::FSparseSet* Pools[] = { Registry.FindStorage<SEntityScriptComponent>().GetSet(),
                Registry.FindStorage<SDisabledTag>().GetSet(), Registry.FindStorage<SScriptDisabledTag>().GetSet() };
            for (int32 Index = 0; Index < (int32)std::size(Pools); ++Index)
            {
                Key.PoolLayouts[Index] = Pools[Index] != nullptr ? *Pools[Index]->GetLayoutEpoch() : 0u;
                Key.PoolSizes[Index]   = Pools[Index] != nullptr ? Pools[Index]->Num() : 0;
            }
            return Key;
        }

        // Claims every entity whose ticking scripts promised to touch nothing else, walking them again only once the key moves.
        void RefreshClaim(ECS::FRegistry& Registry, FClaim& Claim, EClaimPass Pass)
        {
            const FClaimKey Key = MakeClaimKey(Registry);
            if (Claim.bComplete && Claim.Key == Key)
            {
                return;
            }

            LUMINA_PROFILE_SECTION("Claim Entity Scripts");
            Claim.Key = Key;
            SnapshotScriptedEntities(Registry, Claim.Entities);
            const uint32 NumEntities = (uint32)Claim.Entities.Num();
            Claim.Handled.Init(0, NumEntities);
            Claim.Handles.Reset();
            Claim.Scripts.Reset();
            Claim.ScriptEntities.Reset();

            auto ClaimRange = [&](uint32 Begin, uint32 End, bool bCreate, TVector<void*>& Handles, TVector<CEntityScript*>& Scripts, TVector<ECS::FEntity>& Entities)
            {
                FClassFacts Facts;
                bool bNeedsInstance = false;
                for (uint32 EntityIndex = Begin; EntityIndex < End; ++EntityIndex)
                {
                    const EClaimResult Result = ClaimEntity(Registry, Claim.Entities[EntityIndex], Pass, bCreate, Facts, Handles, Scripts, Entities);
                    Claim.Handled[EntityIndex] = (Result == EClaimResult::Claimed || Result == EClaimResult::Idle) ? 1u : 0u;
                    bNeedsInstance |= Result == EClaimResult::NeedsInstance;
                }
                return bNeedsInstance;
            };

            bool bNeedsInstance = false;
            const uint32 NumThreads = Jobs::IsRuntimeRunning() ? Jobs::GetNumThreadSlots() : 0u;
            if (NumEntities >= kParallelClaimMinEntities && NumThreads > 1)
            {
                // An entity still owed a managed instance stays serial, as creating one runs a managed constructor.
                const uint32 NumChunks = (NumEntities + kParallelClaimChunk - 1) / kParallelClaimChunk;
                if ((uint32)Claim.Chunks.Num() < NumChunks)
                {
                    Claim.Chunks.SetNum(NumChunks);
                }
                Jobs::ParallelFor(NumChunks, [&](uint32 Chunk)
                {
                    FClaimChunk& Out = Claim.Chunks[Chunk];
                    Out.Handles.Reset();
                    Out.Scripts.Reset();
                    Out.Entities.Reset();
                    Out.bNeedsInstance = ClaimRange(Chunk * kParallelClaimChunk, Math::Min(NumEntities, (Chunk + 1) * kParallelClaimChunk), false,
                                                    Out.Handles, Out.Scripts, Out.Entities);
                }, 1);

                for (uint32 Chunk = 0; Chunk < NumChunks; ++Chunk)
                {
                    const FClaimChunk& In = Claim.Chunks[Chunk];
                    Claim.Handles.Append(In.Handles.GetData(), In.Handles.Num());
                    Claim.Scripts.Append(In.Scripts.GetData(), In.Scripts.Num());
                    Claim.ScriptEntities.Append(In.Entities.GetData(), In.Entities.Num());
                    bNeedsInstance |= In.bNeedsInstance;
                }
            }
            else
            {
                bNeedsInstance = ClaimRange(0, NumEntities, true, Claim.Handles, Claim.Scripts, Claim.ScriptEntities);
            }

            // A managed constructor run while claiming may have changed things, which leaves every claim suspect.
            if (GStructureEpoch.load(std::memory_order_relaxed) != Key.StructureEpoch)
            {
                Claim.Handled.Init(0, NumEntities);
                Claim.Handles.Reset();
                Claim.Scripts.Reset();
                Claim.ScriptEntities.Reset();
                bNeedsInstance = true;
            }

            // Incomplete, so the next refresh claims again once the serial pass has created the missing instances.
            Claim.bComplete = !bNeedsInstance;
            Claim.NumSerial = (uint32)Algo::Count(Claim.Handled.begin(), Claim.Handled.end(), (uint8)0);
        }
    }

    namespace EntityScripts
    {
        #if USING(WITH_EDITOR)
        void DebugDraw(ECS::FRegistry& Registry)
        {
            CWorld** WorldPtr = Registry.Ctx().Find<CWorld*>();
            if (WorldPtr == nullptr || *WorldPtr == nullptr)
            {
                return;
            }

            CWorld* World = *WorldPtr;
            TVector<ECS::FEntity> Entities;
            FScriptSnapshot Scripts;
            SnapshotScriptedEntities(Registry, Entities);

            for (ECS::FEntity Entity : Entities)
            {
                SnapshotScripts(Registry, Entity, Scripts);
                const bool bSelected = Registry.HasAll<FSelectedInEditorComponent>(Entity);
                for (const TStrongObjectPtr<CEntityScript>& Held : Scripts)
                {
                    CEntityScript* Script = Held.Get();
                    if (Script == nullptr || FEntityScriptAccess::IsFaulted(*Script) || !IsStillAttached(Registry, Entity, Script)
                        || IsOutsideRealm(Script, World))
                    {
                        continue;
                    }

                    // An editor world never attaches its scripts, so the owner is lent for the call and taken back.
                    const bool bLendOwner = !FEntityScriptAccess::IsAttached(*Script);
                    if (bLendOwner)
                    {
                        FEntityScriptAccess::SetOwner(*Script, Entity, World);
                    }

                    Script->OnDebugDraw(bSelected);

                    if (bLendOwner)
                    {
                        FEntityScriptAccess::SetOwner(*Script, ECS::NullEntity, nullptr);
                    }
                }
            }
        }
        #endif

        CEntityScript* Clone(CEntityScript* Source)
        {
            return CloneScript(Source);
        }

        CEntityScript* Attach(ECS::FRegistry& Registry, ECS::FEntity Entity, CClass* ScriptClass)
        {
            if (ScriptClass == nullptr || !ScriptClass->IsChildOf(CEntityScript::StaticClass()))
            {
                LOG_WARN("EntityScripts::Attach: '{}' is not a CEntityScript.",
                    ScriptClass ? *ScriptClass->GetName() : "(null)");
                return nullptr;
            }

            if (ScriptClass->IsAbstract())
            {
                LOG_WARN("EntityScripts::Attach: '{}' is abstract, so attach one of its subclasses instead.", *ScriptClass->GetName());
                return nullptr;
            }

            // find, not get, since a bare registry in a test or tool has no world and its scripts have none.
            CWorld** WorldPtr = Registry.Ctx().Find<CWorld*>();
            if (WorldPtr != nullptr && !NetRealm::Allows(ScriptClass, *WorldPtr))
            {
                return nullptr;
            }

            CObject* Created = NewObject(ScriptClass, nullptr, NAME_None, FGuid::New(), OF_Transient);
            CEntityScript* Script = static_cast<CEntityScript*>(Created);
            if (Script == nullptr)
            {
                return nullptr;
            }

            FEntityScriptAccess::SetOwner(*Script, Entity, WorldPtr != nullptr ? *WorldPtr : nullptr);

            SEntityScriptComponent& Component = Registry.GetOrEmplace<SEntityScriptComponent>(Entity);
            Component.Scripts.Add(Script);
            NoteStructureChange();

            // By the first tick every sibling script added the same frame exists, so OnReady can reference them.
            {
                LUMINA_PROFILE_SECTION_NAMED(*Script->GetClass()->GetName());
                FEntityScriptAccess::BindComponentRefs(*Script);
                Script->OnAttach();
            }
            return Script;
        }

        void NoteStructureChange()
        {
            CheckParallelStructureChange("attaching, detaching or destroying a script");
            BumpStructureEpoch();
        }

        void Tick(ECS::FRegistry& Registry, float DeltaTime, EScriptUpdatePhase Phase)
        {
            // Scripts build meshes one at a time, so their builds are held and run together when the tick ends.
            FMeshBuildBatchScope MeshBatch(Registry);

            const EClaimPass Pass = static_cast<EClaimPass>(Phase);
            FClaimLease Lease(Registry, Pass);
            FClaim& Claim = Lease.Get();
            RefreshClaim(Registry, Claim, Pass);

            // Attach and OnReady are phase-independent, so a PostPhysics script is built in the same pass.
            const bool bDrainLifecycle = Phase == EScriptUpdatePhase::PrePhysics;

            // A C++ subclass runs its own override and a C# one runs the generated shim, indistinguishably.
            CWorld** WorldPtr = Registry.Ctx().Find<CWorld*>();
            CWorld* World = WorldPtr != nullptr ? *WorldPtr : nullptr;

            // Only an entity the claim left serial can owe a lifecycle callback.
            if (bDrainLifecycle && Claim.NumSerial > 0)
            {
                DrainLifecycle(Registry, World);
                RefreshClaim(Registry, Claim, Pass);
            }

            if (Claim.NumSerial == 0 && Claim.Scripts.IsEmpty())
            {
                return;
            }

            FTickScratchGuard        ScratchGuard;
            FTickScratch&            Scratch       = ScratchGuard.Get();
            FScriptSnapshot&         Scripts       = Scratch.Scripts;
            TVector<void*>&          BatchHandles  = Scratch.BatchHandles;
            TVector<ECS::FEntity>&   BatchEntities = Scratch.BatchEntities;
            TVector<CEntityScript*>& BatchScripts  = Scratch.BatchScripts;

            // The unbatched path, which re-checks attachment after every callback exactly as it always has.
            auto RunEntity = [&](ECS::FEntity Entity)
            {
                SnapshotScripts(Registry, Entity, Scripts);

                // Until a callback runs, the snapshot is exactly what the component holds, so there is nothing to re-check.
                bool bUserCodeRan = false;

                for (TStrongObjectPtr<CEntityScript>& Held : Scripts)
                {
                    CEntityScript* Script = Held.Get();
                    if (bUserCodeRan ? !IsStillAttached(Registry, Entity, Script) : Script == nullptr)
                    {
                        continue;
                    }

                    const EScriptUpdatePhase ScriptUpdatePhase = ScriptPhase(Script);

                    // The ordered drain has run by now, so this only catches a script that arrived after its last round.
                    if (bDrainLifecycle && Script->GetOwningEntity() == ECS::NullEntity)
                    {
                        if (!AttachScript(Script, Entity, World))
                        {
                            continue;
                        }
                        bUserCodeRan = true;

                        // OnAttach is user code; it may have detached this very script.
                        if (!IsStillAttached(Registry, Entity, Script))
                        {
                            continue;
                        }
                    }

                    if (FEntityScriptAccess::IsFaulted(*Script))
                    {
                        continue;
                    }

                    if (bDrainLifecycle && !FEntityScriptAccess::IsReady(*Script))
                    {
                        ReadyScript(Script);
                        bUserCodeRan = true;

                        if (!IsStillAttached(Registry, Entity, Script))
                        {
                            continue;
                        }
                    }

                    if (FEntityScriptAccess::ShouldTick(*Script) && ScriptUpdatePhase == Phase)
                    {
                        // Named by class, so a capture splits the script system's time by script and not just in total.
                        LUMINA_PROFILE_SECTION_NAMED(*Script->GetClass()->GetName());
                        Script->OnUpdate(DeltaTime);
                        bUserCodeRan = true;
                    }
                }
            };

            // Queues the entity's updates when every script on it is settled and either C# or idle this phase.
            const CClass*      CachedClass    = nullptr;
            bool               bCachedManaged = false;
            EScriptUpdatePhase CachedPhase    = EScriptUpdatePhase::PrePhysics;
            auto TryQueueEntity = [&](ECS::FEntity Entity) -> bool
            {
                // Read in place rather than snapshotted, since nothing here runs script code that could change the list.
                if (Entity == ECS::NullEntity || !Registry.IsValid(Entity))
                {
                    return false;
                }
                const SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity);
                if (Component == nullptr)
                {
                    return false;
                }

                for (const TStrongObjectPtr<CEntityScript>& Held : Component->Scripts)
                {
                    const CEntityScript* Script = Held.Get();
                    if (Script == nullptr)
                    {
                        return false;
                    }

                    // Attach and OnReady are owed whatever the script's phase, and only the checked path runs them.
                    if (bDrainLifecycle && (Script->GetOwningEntity() == ECS::NullEntity || (!FEntityScriptAccess::IsFaulted(*Script) && !FEntityScriptAccess::IsReady(*Script))))
                    {
                        return false;
                    }

                    // Phase and override are class data, so a run of one class resolves them once.
                    const CClass* Class = Script->GetClass();
                    if (Class != CachedClass)
                    {
                        CachedClass    = Class;
                        bCachedManaged = HasManagedUpdate(Class);
                        CachedPhase    = ScriptPhase(Script);
                    }
                    if (!FEntityScriptAccess::ShouldTick(*Script) || CachedPhase != Phase)
                    {
                        continue;
                    }
                    if (!bCachedManaged)
                    {
                        return false;
                    }
                }

                const int32 Start = BatchHandles.Num();
                const uint32 EpochBefore = GStructureEpoch.load(std::memory_order_relaxed);
                for (const TStrongObjectPtr<CEntityScript>& Held : Component->Scripts)
                {
                    CEntityScript* Script = Held.Get();
                    const EScriptUpdatePhase ScriptUpdatePhase = Script->GetClass() == CachedClass ? CachedPhase : ScriptPhase(Script);
                    if (!FEntityScriptAccess::ShouldTick(*Script) || ScriptUpdatePhase != Phase)
                    {
                        continue;
                    }

                    // Creating the managed instance runs its constructor, which could in principle touch this very list.
                    const uint32 Generation = ManagedInstances::GetHandleGeneration();
                    void* Handle = FEntityScriptAccess::CachedHandleGeneration(*Script) == Generation ? FEntityScriptAccess::CachedManagedHandle(*Script) : nullptr;
                    if (Handle == nullptr)
                    {
                        Handle = Scriptable::GetOrCreateInstance(Script);
                        FEntityScriptAccess::CachedManagedHandle(*Script)    = Handle;
                        FEntityScriptAccess::CachedHandleGeneration(*Script) = Generation;
                    }
                    if (Handle == nullptr || GStructureEpoch.load(std::memory_order_relaxed) != EpochBefore)
                    {
                        BatchHandles.SetNum(Start);
                        BatchEntities.SetNum(Start);
                        BatchScripts.SetNum(Start);
                        return false;
                    }
                    BatchHandles.Add(Handle);
                    BatchEntities.Add(Entity);
                    BatchScripts.Add(Script);
                }
                return true;
            };

            // The epoch the queued entries were validated under, so a change before or during dispatch is caught.
            uint32 QueuedEpoch = 0;

            // Dispatches the first FlushCount entries; a structural change sends everything still queued down the checked path.
            auto FlushBatch = [&](int32 FlushCount)
            {
                const int32 Total = BatchHandles.Num();
                if (FlushCount == 0 || Total == 0)
                {
                    return;
                }

                int32 Ran = 0;
                auto* Dispatch = GDispatchUpdates.Get();
                if (Dispatch != nullptr && GStructureEpoch.load(std::memory_order_relaxed) == QueuedEpoch)
                {
                    const CClass* Class = BatchScripts[0]->GetClass();
                    LUMINA_PROFILE_SECTION_NAMED(*Class->GetName());
                    Ran = Dispatch(BatchHandles.GetData(), FlushCount, DeltaTime, reinterpret_cast<const uint32*>(&GStructureEpoch), nullptr);
                }

                if (Ran == FlushCount && GStructureEpoch.load(std::memory_order_relaxed) == QueuedEpoch)
                {
                    BatchHandles.RemoveAt(0, FlushCount);
                    BatchEntities.RemoveAt(0, FlushCount);
                    BatchScripts.RemoveAt(0, FlushCount);
                    return;
                }

                // Something attached, detached, destroyed or faulted, so the rest goes the checked way.
                int32 Index = Ran;
                if (Ran > 0)
                {
                    const ECS::FEntity Interrupted = BatchEntities[Ran - 1];
                    for (; Index < Total && BatchEntities[Index] == Interrupted; ++Index)
                    {
                        CEntityScript* Script = BatchScripts[Index];
                        if (IsStillAttached(Registry, Interrupted, Script) && FEntityScriptAccess::ShouldTick(*Script) && ScriptPhase(Script) == Phase)
                        {
                            LUMINA_PROFILE_SECTION_NAMED(*Script->GetClass()->GetName());
                            Script->OnUpdate(DeltaTime);
                        }
                    }
                }
                for (ECS::FEntity Previous = ECS::NullEntity; Index < Total; ++Index)
                {
                    if (BatchEntities[Index] != Previous)
                    {
                        Previous = BatchEntities[Index];
                        RunEntity(Previous);
                    }
                }

                BatchHandles.Reset();
                BatchEntities.Reset();
                BatchScripts.Reset();
            };

            auto RunInterrupted = [&](ECS::FEntity Entity, CEntityScript* Script)
            {
                if (IsStillAttached(Registry, Entity, Script) && FEntityScriptAccess::ShouldTick(*Script) && ScriptPhase(Script) == Phase)
                {
                    LUMINA_PROFILE_SECTION_NAMED(*Script->GetClass()->GetName());
                    Script->OnUpdate(DeltaTime);
                }
            };

            // Entities whose ticking scripts all promised to touch nothing else run first, spread across the workers.
            if (!Claim.Scripts.IsEmpty())
            {
                LUMINA_PROFILE_SECTION("Parallel Entity Scripts");
                RunInEntityChunks(Registry, Scratch.ChunkStarts, Scratch.ChunkRan, Claim.ScriptEntities, Claim.Scripts, [&](uint32 Begin, uint32 End)
                {
                    return RunClaimedSpan(Claim.Handles.GetData() + Begin, Claim.Scripts.GetData() + Begin, End - Begin, DeltaTime, Claim.Key.StructureEpoch);
                }, RunInterrupted, RunEntity);
            }

            const bool bProfilerAttached = TracyIsConnected;
            for (int32 EntityIndex = 0; Claim.NumSerial > 0 && EntityIndex < Claim.Entities.Num(); ++EntityIndex)
            {
                if (Claim.Handled[EntityIndex] != 0)
                {
                    continue;
                }
                const ECS::FEntity Entity = Claim.Entities[EntityIndex];
                const int32 QueuedBefore = BatchHandles.Num();
                if (QueuedBefore == 0)
                {
                    QueuedEpoch = GStructureEpoch.load(std::memory_order_relaxed);
                }

                if (TryQueueEntity(Entity))
                {
                    // A connected profiler still splits the time by class, so a run never mixes two.
                    if (bProfilerAttached && QueuedBefore > 0 && BatchHandles.Num() > QueuedBefore
                        && BatchScripts[QueuedBefore]->GetClass() != BatchScripts[0]->GetClass())
                    {
                        FlushBatch(QueuedBefore);
                    }
                    continue;
                }

                FlushBatch(BatchHandles.Num());
                RunEntity(Entity);
            }
            FlushBatch(BatchHandles.Num());

            // Parked storage would otherwise hold the last entity's scripts alive until the next tick.
            Scripts.Reset();
        }

        void TickFixed(ECS::FRegistry& Registry, float FixedDeltaTime)
        {
            // The same split as Tick, without the managed batching, since a fixed step crosses per script either way.
            FClaimLease Lease(Registry, EClaimPass::Fixed);
            FClaim& Claim = Lease.Get();
            RefreshClaim(Registry, Claim, EClaimPass::Fixed);
            if (Claim.NumSerial == 0 && Claim.Scripts.IsEmpty())
            {
                return;
            }

            FTickScratchGuard ScratchGuard;
            FTickScratch&     Scratch = ScratchGuard.Get();
            FScriptSnapshot&  Scripts = Scratch.Scripts;

            auto RunEntity = [&](ECS::FEntity Entity)
            {
                SnapshotScripts(Registry, Entity, Scripts);
                for (TStrongObjectPtr<CEntityScript>& Held : Scripts)
                {
                    // Fixed update only runs on a readied script, so none sees a fixed step before its OnReady.
                    CEntityScript* Script = Held.Get();
                    if (Script != nullptr && FEntityScriptAccess::ShouldTick(*Script) && IsStillAttached(Registry, Entity, Script))
                    {
                        Script->OnFixedUpdate(FixedDeltaTime);
                    }
                }
            };

            if (!Claim.Scripts.IsEmpty())
            {
                LUMINA_PROFILE_SECTION("Parallel Entity Scripts Fixed");
                RunInEntityChunks(Registry, Scratch.ChunkStarts, Scratch.ChunkRan, Claim.ScriptEntities, Claim.Scripts, [&](uint32 Begin, uint32 End)
                {
                    uint32 Index = Begin;
                    for (; Index < End && GStructureEpoch.load(std::memory_order_relaxed) == Claim.Key.StructureEpoch; ++Index)
                    {
                        WatchParallelRun(Claim.Scripts.GetData() + Index, nullptr);
                        Claim.Scripts[Index]->OnFixedUpdate(FixedDeltaTime);
                    }
                    return Index - Begin;
                }, [&](ECS::FEntity Entity, CEntityScript* Script)
                {
                    if (IsStillAttached(Registry, Entity, Script) && FEntityScriptAccess::ShouldTick(*Script))
                    {
                        Script->OnFixedUpdate(FixedDeltaTime);
                    }
                }, RunEntity);
            }

            for (int32 EntityIndex = 0; EntityIndex < Claim.Entities.Num(); ++EntityIndex)
            {
                if (Claim.Handled[EntityIndex] == 0)
                {
                    RunEntity(Claim.Entities[EntityIndex]);
                }
            }

            // Parked storage would otherwise hold the last entity's scripts alive until the next tick.
            Scripts.Reset();
        }

        CEntityScript* Find(ECS::FRegistry& Registry, ECS::FEntity Entity, const CClass* ScriptClass)
        {
            if (ScriptClass == nullptr)
            {
                return nullptr;
            }
            SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity);
            if (Component == nullptr)
            {
                return nullptr;
            }
            for (TStrongObjectPtr<CEntityScript>& Held : Component->Scripts)
            {
                CEntityScript* Script = Held.Get();
                if (Script != nullptr && Script->GetClass() != nullptr && Script->GetClass()->IsChildOf(ScriptClass))
                {
                    return Script;
                }
            }
            return nullptr;
        }

        void FindAll(ECS::FRegistry& Registry, ECS::FEntity Entity, const CClass* ScriptClass,
            TVector<CEntityScript*>& Out)
        {
            if (ScriptClass == nullptr)
            {
                return;
            }
            SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity);
            if (Component == nullptr)
            {
                return;
            }
            for (TStrongObjectPtr<CEntityScript>& Held : Component->Scripts)
            {
                CEntityScript* Script = Held.Get();
                if (Script != nullptr && Script->GetClass() != nullptr && Script->GetClass()->IsChildOf(ScriptClass))
                {
                    Out.Add(Script);
                }
            }
        }

        bool Remove(ECS::FRegistry& Registry, ECS::FEntity Entity, CEntityScript* Script)
        {
            if (Script == nullptr || !IsStillAttached(Registry, Entity, Script))
            {
                return false;
            }
            
            TStrongObjectPtr<CEntityScript> Pinned(Script);
            NoteStructureChange();

            if (FEntityScriptAccess::IsAttached(*Script))
            {
                Script->OnDetach();
            }
            FEntityScriptAccess::EndBindings(*Script);
            UIScripts::Close(Script);
            
            SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity);
            if (Component == nullptr)
            {
                return true;   // OnDetach tore the component down; the script is gone either way
            }

            for (auto It = Component->Scripts.begin(); It != Component->Scripts.end(); ++It)
            {
                if (It->Get() == Script)
                {
                    Component->Scripts.RemoveAt(static_cast<int32>(It - Component->Scripts.begin()));
                    return true;
                }
            }
            return true;
        }

        namespace
        {
            // Takes the snapshot buffer from the caller, so a registry-wide teardown reuses one allocation.
            void DetachAllInto(ECS::FRegistry& Registry, ECS::FEntity Entity, FScriptSnapshot& Scripts)
            {
                SnapshotScripts(Registry, Entity, Scripts);
                if (Scripts.IsEmpty())
                {
                    return;
                }
                NoteStructureChange();

                for (TStrongObjectPtr<CEntityScript>& Held : Scripts)
                {
                    // For a C# script the call would mint a managed instance purely to tear it down again.
                    if (CEntityScript* Script = Held.Get(); Script != nullptr)
                    {
                        if (FEntityScriptAccess::IsAttached(*Script))
                        {
                            Script->OnDetach();
                        }
                        FEntityScriptAccess::EndBindings(*Script);
                    }
                    UIScripts::Close(Held.Get());
                }

                if (SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity))
                {
                    Component->Scripts.Reset();
                }
            }
        }

        void DetachAll(ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            FScriptSnapshot Scripts;
            DetachAllInto(Registry, Entity, Scripts);
        }

        namespace
        {
            // A live script the reload owes OnReloaded, gathered so the calls run after the storage walk.
            struct FReloadTarget
            {
                ECS::FEntity                     Entity;
                TStrongObjectPtr<CEntityScript>  Script;
            };

            // One walk of the script storage holds scripts whose class the reload removed, restores held ones whose class is back, and lists the C# scripts owed OnReloaded.
            void ReconcileScripts(ECS::FRegistry& Registry, TVector<FReloadTarget>* OutTargets)
            {
                // Missing components restore structurally, so they go before the walk, which must not add storages.
                MissingComponents::HoldOrphaned(Registry);
                MissingComponents::Restore(Registry);

                TVector<TStrongObjectPtr<CEntityScript>> Released;
                int32 Restored = 0;

                Registry.View<SEntityScriptComponent>().ForEach([&](ECS::FEntity Entity, SEntityScriptComponent& Component)
                {
                    // A removed class builds nothing new, so its live scripts become held values that copies, undo and saves carry until it returns.
                    Component.Scripts.RemoveAll([&](TStrongObjectPtr<CEntityScript>& Script)
                    {
                        CClass* Class = Script.Get() != nullptr ? Script->GetClass() : nullptr;
                        if (Class == nullptr || !Class->IsSuperseded())
                        {
                            return false;
                        }
                        SHeldValue& Held = Component.Pending.EmplaceGetRef();
                        Held.TypeName = Class->GetName();
                        Held.Data.Capture(*Class, Script.Get());
                        Released.Add(Move(Script));
                        return true;
                    });

                    if (!Component.Pending.IsEmpty())
                    {
                        Restored += Component.Pending.RemoveAll([&Component](const SHeldValue& Held)
                        {
                            CEntityScript* Script = RestoreHeldScript(Held);
                            if (Script != nullptr)
                            {
                                Component.Scripts.Add(Script);
                                LOG_INFO("SEntityScriptComponent: restored held script '{}' now that its class is loaded.", *Held.TypeName);
                            }
                            return Script != nullptr;
                        });
                    }

                    if (OutTargets == nullptr)
                    {
                        return;
                    }
                    // A C++ script's instance never went anywhere, and a restored one has not attached yet.
                    for (const TStrongObjectPtr<CEntityScript>& Script : Component.Scripts)
                    {
                        if (Script.Get() != nullptr && FEntityScriptAccess::IsAttached(*Script) && ToScriptClass(Script->GetClass()) != nullptr)
                        {
                            OutTargets->Add({ Entity, Script });
                        }
                    }
                });

                if (Restored > 0 || !Released.IsEmpty())
                {
                    NoteStructureChange();
                }
                if (Released.IsEmpty())
                {
                    return;
                }

                // Its managed type is gone, so no OnDetach can run, and only the native side is torn down.
                for (TStrongObjectPtr<CEntityScript>& Script : Released)
                {
                    UIScripts::Close(Script.Get());
                }
                LOG_WARN("SEntityScriptComponent: {} script(s) of a removed class are held until it comes back.", Released.Num());
            }
        }

        void NotifyScriptsReloaded(EScriptReloadReason Reason, int32 Generation)
        {
            // A reload may change a class's phase or promise in place, which every cached claim was built on.
            NoteStructureChange();

            if (Global::GWorldManager == nullptr)
            {
                return;
            }

            // A prefab loaded before its script class keeps that script held, and every spawn would copy it held.
            for (TObjectIterator<CPrefab> It; It; ++It)
            {
                CPrefab* Prefab = *It;
                ReconcileScripts(Prefab->Registry, nullptr);
                ReconcileScripts(Prefab->VariantDelta, nullptr);
            }

            // Reused across worlds, so a reload allocates once however many scripted entities there are.
            TVector<FReloadTarget> Targets;
            Global::GWorldManager->ForEachWorld([&](CWorld& World)
            {
                ECS::FRegistry& Registry = World.GetEntityRegistry();

                // Anything held back by a failed or late load comes in first, so it sees this reload too.
                Targets.Reset();
                ReconcileScripts(Registry, &Targets);

                SScriptReloadContext Context;
                Context.Reason     = Reason;
                Context.WorldType  = World.GetWorldType();
                Context.Generation = Generation;

                // An earlier handler may detach or destroy what a later target belongs to.
                for (const FReloadTarget& Target : Targets)
                {
                    if (IsStillAttached(Registry, Target.Entity, Target.Script.Get()))
                    {
                        Context.Entity = Target.Entity;
                        Target.Script->OnReloaded(Context);
                    }
                }
            });
        }

        void DetachAllInRegistry(ECS::FRegistry& Registry)
        {
            // Disabled entities are included, since a disabled script still ran OnAttach and is owed its OnDetach.
            TVector<ECS::FEntity> Entities;
            auto View = Registry.View<SEntityScriptComponent>();
            Entities.Reserve(View.Num());
            for (ECS::FEntity Entity : View)
            {
                Entities.Add(Entity);
            }

            FScriptSnapshot Scripts;
            for (ECS::FEntity Entity : Entities)
            {
                if (Registry.IsValid(Entity))
                {
                    DetachAllInto(Registry, Entity, Scripts);
                }
            }
        }
    }
}
