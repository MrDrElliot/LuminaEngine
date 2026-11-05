#include "RuntimePCH.h"
#include "Networking/NetRealm.h"
#include "EntitySystem.h"
#include "ExclusiveSystem.h"

#include "Core/Object/Class.h"
#include "Core/Object/ObjectBase.h"
#include "Core/Object/ObjectIteration.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ScriptClass.h"
#include "Log/Log.h"
#include "World/ECS/EventQueue.h"
#include "World/Entity/Components/Component.h"
#include "World/World.h"

namespace Lumina
{
    namespace EntitySystemInternal
    {
        // The system whose members are being built, bounded by its own bytes so nothing else built meanwhile registers with it.
        struct FConstructingSystem
        {
            CEntitySystem*                 System = nullptr;
            const uint8*                   Begin  = nullptr;
            const uint8*                   End    = nullptr;
            const FConstructCObjectParams* Scope  = nullptr;
        };

        thread_local FConstructingSystem GConstructing;

        template<typename T>
        void AddUnique(TVector<T>& Ids, const T& Id)
        {
            if (!Algo::Contains(Ids, Id))
            {
                Ids.Add(Id);
            }
        }

        enum class EManagedParamKind : uint32
        {
            Bound       = 0,
            Query       = 1,
            EventReader = 2,
        };

        // Mirrors SystemParamBlock in LuminaSharp/World/SystemParams.cs, which owns the memory for as long as its system lives.
        struct FManagedSystemParam
        {
            static constexpr int32 MaxIncludes = 8;
            static constexpr int32 MaxExcludes = 4;
            static constexpr int32 MaxFilters  = 4;
            static constexpr int32 MaxRequires = 4;

            CWorld*              World       = nullptr;
            uint64               Cursor      = 0;
            EManagedParamKind    Kind        = EManagedParamKind::Bound;
            uint32               EventType   = 0;
            int32                NumIncludes = 0;
            uint32               LastRunTick = 0;
            const FComponentOps* Includes[MaxIncludes] = {};
            uint32               WriteMask   = 0;
            uint32               AddedMask   = 0;
            int32                NumExcludes = 0;
            int32                NumFilters  = 0;
            const FComponentOps* Excludes[MaxExcludes] = {};
            const FComponentOps* Filters[MaxFilters] = {};
            const FComponentOps* Requires[MaxRequires] = {};
            int32                NumRequires = 0;
        };

        static_assert(offsetof(FManagedSystemParam, Cursor) == 8 && offsetof(FManagedSystemParam, Kind) == 16
            && offsetof(FManagedSystemParam, EventType) == 20 && offsetof(FManagedSystemParam, NumIncludes) == 24
            && offsetof(FManagedSystemParam, LastRunTick) == 28 && offsetof(FManagedSystemParam, Includes) == 32
            && offsetof(FManagedSystemParam, WriteMask) == 96 && offsetof(FManagedSystemParam, AddedMask) == 100
            && offsetof(FManagedSystemParam, NumExcludes) == 104 && offsetof(FManagedSystemParam, NumFilters) == 108
            && offsetof(FManagedSystemParam, Excludes) == 112 && offsetof(FManagedSystemParam, Filters) == 144
            && offsetof(FManagedSystemParam, Requires) == 176 && offsetof(FManagedSystemParam, NumRequires) == 208
            && sizeof(FManagedSystemParam) == 216);

        // As TQuery declares its terms: each include by its access, each filter as a tracked read, and an exclude not at all.
        void DeclareManagedQuery(const FManagedSystemParam& Param, FSystemAccess& Access)
        {
            const int32 NumIncludes = Param.NumIncludes < FManagedSystemParam::MaxIncludes ? Param.NumIncludes : FManagedSystemParam::MaxIncludes;
            for (int32 Index = 0; Index < NumIncludes; ++Index)
            {
                if (const FComponentOps* Ops = Param.Includes[Index])
                {
                    const uint32 Id = static_cast<uint32>(Ops->TypeId);
                    (Param.WriteMask & (1u << Index)) != 0 ? DeclareParamWrite(Access, Id, true) : DeclareParamRead(Access, Id, true);
                }
            }

            const int32 NumFilters = Param.NumFilters < FManagedSystemParam::MaxFilters ? Param.NumFilters : FManagedSystemParam::MaxFilters;
            for (int32 Index = 0; Index < NumFilters; ++Index)
            {
                if (const FComponentOps* Ops = Param.Filters[Index])
                {
                    const uint32 Id = static_cast<uint32>(Ops->TypeId);
                    DeclareParamRead(Access, Id, true);
                    DeclareParamTracked(Access, Id);
                }
            }
        }

        bool ManagedQueryHasWork(const FManagedSystemParam& Param)
        {
            if (Param.World == nullptr || Param.NumIncludes <= 0)
            {
                return false;
            }

            const ECS::FRegistry& Registry = GetSystemRegistry(*Param.World);
            const auto HasAny = [&Registry](const FComponentOps* Ops)
            {
                const ECS::FSparseSet* Storage = Ops != nullptr ? Registry.FindStorage(static_cast<uint32>(Ops->TypeId)) : nullptr;
                return Storage != nullptr && Storage->Num() != 0;
            };

            const int32 NumIncludes = Param.NumIncludes < FManagedSystemParam::MaxIncludes ? Param.NumIncludes : FManagedSystemParam::MaxIncludes;
            for (int32 Index = 0; Index < NumIncludes; ++Index)
            {
                if (!HasAny(Param.Includes[Index]))
                {
                    return false;
                }
            }

            const int32 NumRequires = Param.NumRequires < FManagedSystemParam::MaxRequires ? Param.NumRequires : FManagedSystemParam::MaxRequires;
            for (int32 Index = 0; Index < NumRequires; ++Index)
            {
                if (!HasAny(Param.Requires[Index]))
                {
                    return false;
                }
            }
            return true;
        }

        bool ManagedReaderHasWork(const FManagedSystemParam& Param)
        {
            if (Param.World == nullptr)
            {
                return false;
            }
            const ECS::FEventQueue* Queue = GetSystemEvents(*Param.World).Find(static_cast<ECS::FComponentTypeID>(Param.EventType));
            return Queue != nullptr && Queue->NumUnread(Param.Cursor) != 0;
        }

        const FSystemParamOps GManagedBoundOps
        {
            .Bind = [](void* Param, CWorld& World) { static_cast<FManagedSystemParam*>(Param)->World = &World; },
        };

        const FSystemParamOps GManagedWorkOps
        {
            .Bind    = [](void* Param, CWorld& World) { static_cast<FManagedSystemParam*>(Param)->World = &World; },
            .HasWork = [](const void* Param)
            {
                const FManagedSystemParam& Managed = *static_cast<const FManagedSystemParam*>(Param);
                return Managed.Kind == EManagedParamKind::EventReader ? ManagedReaderHasWork(Managed) : ManagedQueryHasWork(Managed);
            },
        };

        // Exact rather than IsChildOf, so a system deriving from another still gets its own instance.
        bool AlreadyPresent(const TVector<TStrongObjectPtr<CEntitySystem>>& Systems, const CClass* Class)
        {
            for (const TStrongObjectPtr<CEntitySystem>& System : Systems)
            {
                if (System != nullptr && System->GetClass() == Class)
                {
                    return true;
                }
            }
            return false;
        }

        void DropWhere(const SSystemContext& Context, TVector<TStrongObjectPtr<CEntitySystem>>& Systems, TFunctionRef<bool(const CEntitySystem&)> Predicate)
        {
            // Back to front, so a system created later can still reach an earlier one while it tears down.
            for (int32 Index = Systems.Num(); Index > 0; --Index)
            {
                CEntitySystem* System = Systems[Index - 1].Get();
                if (System == nullptr || !Predicate(*System))
                {
                    continue;
                }

                // Paired with OnStartup, so a system dropped before the world started never sees one.
                if (System->HasStarted())
                {
                    System->OnTeardown(SExclusiveContext(Context));
                }
                System->UnbindParams();
                System->SetOwningWorld(nullptr);
                Systems.RemoveAt((int64)(Index - 1));
            }
        }
    }

    CEntitySystem::CEntitySystem()
    {
        const FConstructCObjectParams* Scope = FScopedObjectConstruction::Current();
        const uint32 Size = Scope != nullptr && Scope->Class != nullptr ? Scope->Class->GetSize() : static_cast<uint32>(sizeof(CEntitySystem));
        const uint8* Begin = reinterpret_cast<const uint8*>(this);
        EntitySystemInternal::GConstructing = EntitySystemInternal::FConstructingSystem{ this, Begin, Begin + Size, Scope };
    }

    void RegisterSystemParam(void* Param, const FSystemParamOps& Ops)
    {
        const EntitySystemInternal::FConstructingSystem& Constructing = EntitySystemInternal::GConstructing;
        const uint8* At = static_cast<const uint8*>(Param);
        if (Constructing.System == nullptr || At < Constructing.Begin || At >= Constructing.End
            || FScopedObjectConstruction::Current() != Constructing.Scope)
        {
            return;
        }
        Constructing.System->Params.Add(CEntitySystem::FRegisteredParam{ Param, &Ops });
    }

    ECS::FRegistry& GetSystemRegistry(CWorld& World)
    {
        return World.GetEntityRegistry();
    }

    ECS::FEventQueues& GetSystemEvents(CWorld& World)
    {
        return World.GetEventQueues();
    }

    void DeclareParamRead(FSystemAccess& Access, uint32 TypeId, bool bComponent, int32 Membership)
    {
        EntitySystemInternal::AddUnique(Access.Reads, TypeId);
        if (bComponent)
        {
            EntitySystemInternal::AddUnique(Access.ComponentAccesses, FComponentAccess{ TypeId, Membership, false });
            if (!Algo::Contains(Access.Pools, static_cast<ECS::FComponentTypeID>(TypeId)))
            {
                Access.Pools.Add(static_cast<ECS::FComponentTypeID>(TypeId));
            }
        }
    }

    void DeclareParamWrite(FSystemAccess& Access, uint32 TypeId, bool bComponent, int32 Membership)
    {
        EntitySystemInternal::AddUnique(Access.Writes, TypeId);
        if (bComponent)
        {
            EntitySystemInternal::AddUnique(Access.ComponentAccesses, FComponentAccess{ TypeId, Membership, true });
            if (!Algo::Contains(Access.Pools, static_cast<ECS::FComponentTypeID>(TypeId)))
            {
                Access.Pools.Add(static_cast<ECS::FComponentTypeID>(TypeId));
            }
        }
    }

    int32 DeclareParamMembership(FSystemAccess& Access, const uint32* With, int32 NumWith, const uint32* Without, int32 NumWithout)
    {
        FAccessMembership Membership;
        Membership.With.Append(With, With + NumWith);
        Membership.Without.Append(Without, Without + NumWithout);
        Access.Memberships.Add(std::move(Membership));
        return Access.Memberships.Num() - 1;
    }

    void DeclareParamPool(FSystemAccess& Access, uint32 TypeId)
    {
        if (!Algo::Contains(Access.Pools, static_cast<ECS::FComponentTypeID>(TypeId)))
        {
            Access.Pools.Add(static_cast<ECS::FComponentTypeID>(TypeId));
        }
    }

    void DeclareParamTracked(FSystemAccess& Access, uint32 TypeId)
    {
        if (!Algo::Contains(Access.TrackedPools, static_cast<ECS::FComponentTypeID>(TypeId)))
        {
            Access.TrackedPools.Add(static_cast<ECS::FComponentTypeID>(TypeId));
        }
    }

    void DeclareParamSends(FSystemAccess& Access, uint32 TypeId)
    {
        EntitySystemInternal::AddUnique(Access.Writes, TypeId);
        EntitySystemInternal::AddUnique(Access.SentEvents, TypeId);
    }

    bool CEntitySystem::HasWork(const SSystemContext&)
    {
        bool bAnySay = false;
        for (const FRegisteredParam& Registered : Params)
        {
            if (Registered.Ops->HasWork != nullptr)
            {
                bAnySay = true;
                if (Registered.Ops->HasWork(Registered.Param))
                {
                    return true;
                }
            }
        }
        return !bAnySay;
    }

    void CExclusiveSystem::OnUpdate(const SSystemContext& Context)
    {
        OnExclusiveUpdate(SExclusiveContext(Context));
    }

    void CEntitySystem::RequireUpdate(EUpdateStage Stage)
    {
        if (Stage < EUpdateStage::Max)
        {
            StageMask |= (uint8)(1u << (uint32)Stage);
        }
    }

    void CEntitySystem::RunsAfterSystem(FName System)
    {
        if (!Algo::Contains(After, System))
        {
            After.Add(System);
        }
    }

    void CEntitySystem::RunsBeforeSystem(FName System)
    {
        if (!Algo::Contains(Before, System))
        {
            Before.Add(System);
        }
    }

    void CEntitySystem::DeclareWrite(FName Component)
    {
        ECS::FResourceTypeRegistry& Resources = ECS::FResourceTypeRegistry::Get();
        if (const FComponentOps* Ops = FindComponentOps(Component.ToString().View()))
        {
            DeclareParamWrite(Access, static_cast<uint32>(Ops->TypeId), true);
        }
        else if (const ECS::FResourceType* Resource = Resources.Find(Component))
        {
            DeclareParamWrite(Access, Resource->TypeId, false);
        }
        else if (const ECS::FComponentTypeID Event = ECS::FindEventType(Component); Event != ECS::InvalidComponentTypeID)
        {
            Access.Writes.Add((uint32)Event);
            Access.SentEvents.Add((uint32)Event);
        }
        else if (const uint32 Token = Resources.FindNamedAccessToken(Component); Token != 0)
        {
            DeclareParamWrite(Access, Token, false);
        }
        else
        {
            LOG_WARN("Entity system '{}' declares a write of unknown component, resource or event '{}'.", GetClass()->GetName(), Component);
        }
    }

    void CEntitySystem::DeclareRead(FName Component)
    {
        ECS::FResourceTypeRegistry& Resources = ECS::FResourceTypeRegistry::Get();
        if (const FComponentOps* Ops = FindComponentOps(Component.ToString().View()))
        {
            DeclareParamRead(Access, static_cast<uint32>(Ops->TypeId), true);
        }
        else if (const ECS::FResourceType* Resource = Resources.Find(Component))
        {
            DeclareParamRead(Access, Resource->TypeId, false);
        }
        else if (const ECS::FComponentTypeID Event = ECS::FindEventType(Component); Event != ECS::InvalidComponentTypeID)
        {
            Access.Reads.Add((uint32)Event);
        }
        else if (const uint32 Token = Resources.FindNamedAccessToken(Component); Token != 0)
        {
            DeclareParamRead(Access, Token, false);
        }
        else
        {
            LOG_WARN("Entity system '{}' declares a read of unknown component, resource or event '{}'.", GetClass()->GetName(), Component);
        }
    }

    void CEntitySystem::DeclareManagedParam(void* Param)
    {
        if (Param == nullptr)
        {
            return;
        }

        EntitySystemInternal::FManagedSystemParam* Managed = static_cast<EntitySystemInternal::FManagedSystemParam*>(Param);
        Managed->World = OwningWorld;
        if (Managed->Kind == EntitySystemInternal::EManagedParamKind::Query)
        {
            EntitySystemInternal::DeclareManagedQuery(*Managed, Access);
        }
        if (Algo::AnyOf(Params, [Param](const FRegisteredParam& Registered) { return Registered.Param == Param; }))
        {
            return;
        }

        const bool bHasWork = Managed->Kind == EntitySystemInternal::EManagedParamKind::Query || Managed->Kind == EntitySystemInternal::EManagedParamKind::EventReader;
        Params.Add(FRegisteredParam{ Param, bHasWork ? &EntitySystemInternal::GManagedWorkOps : &EntitySystemInternal::GManagedBoundOps });
    }

    void CEntitySystem::UnbindParams()
    {
        for (const FRegisteredParam& Registered : Params)
        {
            if (Registered.Ops->Unbind != nullptr)
            {
                Registered.Ops->Unbind(Registered.Param);
            }
        }
    }

    void CEntitySystem::ConfigureSystem()
    {
        StageMask = 0;
        After.Reset();
        Before.Reset();
        Access = FSystemAccess();
        Access.bExclusive = bExclusiveClass;

        if (OwningWorld != nullptr)
        {
            for (const FRegisteredParam& Registered : Params)
            {
                if (Registered.Ops->Bind != nullptr)
                {
                    Registered.Ops->Bind(Registered.Param, *OwningWorld);
                }
            }
        }

        Configure();

        for (const FRegisteredParam& Registered : Params)
        {
            if (Registered.Ops->Declare != nullptr)
            {
                Registered.Ops->Declare(Registered.Param, Access);
            }
        }

        // This system is built, so a member constructed later never mistakes it for its owner.
        EntitySystemInternal::GConstructing = EntitySystemInternal::FConstructingSystem();
    }

    namespace EntitySystems
    {
        void ForEachSystemClass(TFunctionRef<void(CClass*)> Visitor)
        {
            CClass* BaseClass = CEntitySystem::StaticClass();

            // Collected before anything is visited, because creating a CDO mutates the object array.
            TVector<CClass*> Classes;
            ForEachObject([&](CObjectBase* Object, int32)
            {
                if (Object == nullptr || !Object->IsA<CClass>())
                {
                    return;
                }

                CClass* Class = static_cast<CClass*>(Object);
                if (Class != BaseClass && Class->IsChildOf(BaseClass) && Class->CanInstantiate())
                {
                    Classes.Add(Class);
                }
            });

            for (CClass* Class : Classes)
            {
                Visitor(Class);
            }
        }

        int32 CreateMissing(CWorld& World, const THashSet<FName>& Disabled, TVector<TStrongObjectPtr<CEntitySystem>>& Out)
        {
            const int32 FirstNew = Out.Num();

            ForEachSystemClass([&](CClass* Class)
            {
                if (Disabled.Contains(Class->GetName()) != 0 || EntitySystemInternal::AlreadyPresent(Out, Class))
                {
                    return;
                }

                if (!NetRealm::Allows(Class, &World))
                {
                    return;
                }

                CEntitySystem* Defaults = Class->GetDefaultObject<CEntitySystem>();
                if (Defaults == nullptr || !Defaults->ShouldCreate())
                {
                    return;
                }

                CEntitySystem* System = NewObject<CEntitySystem>(Class);
                if (System == nullptr)
                {
                    LOG_WARN("Entity system '{}' could not be created.", Class->GetName());
                    return;
                }

                System->SetOwningWorld(&World);
                System->ConfigureSystem();
                Out.Add(System);
            });

            return Out.Num() - FirstNew;
        }

        void DropScripted(const SSystemContext& Context, TVector<TStrongObjectPtr<CEntitySystem>>& Systems)
        {
            EntitySystemInternal::DropWhere(Context, Systems, [](const CEntitySystem& System)
            {
                return Cast<CScriptClass>(System.GetClass()) != nullptr;
            });
        }

        void DropOutsideRealm(const SSystemContext& Context, CWorld& World, TVector<TStrongObjectPtr<CEntitySystem>>& Systems)
        {
            EntitySystemInternal::DropWhere(Context, Systems, [&World](const CEntitySystem& System)
            {
                return !NetRealm::Allows(System.GetClass(), &World);
            });
        }

        void DropDisabled(const SSystemContext& Context, const THashSet<FName>& Disabled, TVector<TStrongObjectPtr<CEntitySystem>>& Systems)
        {
            EntitySystemInternal::DropWhere(Context, Systems, [&Disabled](const CEntitySystem& System)
            {
                return Disabled.Contains(System.GetClass()->GetName()) != 0;
            });
        }

        void StartupPending(const SSystemContext& Context, TVector<TStrongObjectPtr<CEntitySystem>>& Systems)
        {
            for (TStrongObjectPtr<CEntitySystem>& System : Systems)
            {
                if (System != nullptr && !System->HasStarted())
                {
                    System->MarkStarted();
                    System->OnStartup(SExclusiveContext(Context));
                }
            }
        }

        CEntitySystem* Find(const TVector<TStrongObjectPtr<CEntitySystem>>& Systems, const CClass* Class)
        {
            if (Class == nullptr)
            {
                return nullptr;
            }

            for (const TStrongObjectPtr<CEntitySystem>& System : Systems)
            {
                if (System != nullptr && System->GetClass()->IsChildOf(Class))
                {
                    return System.Get();
                }
            }

            return nullptr;
        }

        void DestroyAll(const SSystemContext& Context, TVector<TStrongObjectPtr<CEntitySystem>>& Systems)
        {
            // Back to front, so a system created later can still reach an earlier one while it tears down.
            for (int32 Index = Systems.Num(); Index > 0; --Index)
            {
                if (CEntitySystem* System = Systems[Index - 1].Get())
                {
                    if (System->HasStarted())
                    {
                        System->OnTeardown(SExclusiveContext(Context));
                    }
                    System->UnbindParams();
                    System->SetOwningWorld(nullptr);
                }
            }

            Systems.Reset();
        }
    }
}
