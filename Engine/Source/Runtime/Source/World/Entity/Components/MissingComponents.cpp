#include "RuntimePCH.h"
#include "MissingComponents.h"

#include "Component.h"
#include "Scripting/ScriptStruct.h"
#include "Scripting/ScriptTypeRegistry.h"
#include "World/Entity/EntityUtils.h"

namespace Lumina
{
    void SHeldValue::RemapEntities(const THashMap<ECS::FEntity, ECS::FEntity>& Map, bool bClearUnmapped)
    {
        const uint32 Null = static_cast<ECS::FEntity>(ECS::NullEntity).Value;
        for (uint32& To : RemapTo)
        {
            if (To == Null)
            {
                continue;
            }
            if (const auto It = Map.Find(ECS::FEntity(To)); It != nullptr)
            {
                To = It->Value;
            }
            else if (bClearUnmapped)
            {
                To = Null;
            }
        }

        // A handle no earlier copy remapped still names its original entity, so this map applies to it directly.
        if (!bClearUnmappedEntities)
        {
            THashSet<uint32> Remapped;
            Remapped.Append(RemapFrom);
            for (const auto& [From, To] : Map)
            {
                if (!Remapped.Contains(From.Value))
                {
                    RemapFrom.Add(From.Value);
                    RemapTo.Add(To.Value);
                }
            }
            bClearUnmappedEntities = bClearUnmapped;
        }
    }

    void SHeldValue::ApplyEntityRemap(const CStruct* Struct, void* Value) const
    {
        if (RemapFrom.IsEmpty() && !bClearUnmappedEntities)
        {
            return;
        }
        THashMap<ECS::FEntity, ECS::FEntity> Map;
        const int32 Pairs = Math::Min(RemapFrom.Num(), RemapTo.Num());
        for (int32 Index = 0; Index < Pairs; ++Index)
        {
            Map.TryAdd(ECS::FEntity(RemapFrom[Index]), ECS::FEntity(RemapTo[Index]));
        }
        ECS::Utils::RemapEntityReferences(Struct, Value, Map, bClearUnmappedEntities);
    }
}

namespace Lumina::MissingComponents
{
    void Hold(ECS::FRegistry& Registry, ECS::FEntity Entity, SHeldValue&& Held)
    {
        SMissingComponents& Holder = Registry.GetOrEmplace<SMissingComponents>(Entity);
        for (SHeldValue& Existing : Holder.Entries)
        {
            if (Existing.TypeName == Held.TypeName)
            {
                Existing = Move(Held);
                return;
            }
        }
        Holder.Entries.Add(Move(Held));
    }

    int32 Restore(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        SMissingComponents* Holder = Registry.TryGet<SMissingComponents>(Entity);
        if (Holder == nullptr)
        {
            return 0;
        }

        // Moved out first, since emplacing fires construct signals that may touch the holder.
        TVector<SHeldValue> Holding = Move(Holder->Entries);
        int32 Restored = 0;
        Holding.RemoveAll([&](const SHeldValue& Held)
        {
            CStruct* Struct = ResolveStructByName(Held.TypeName);
            const FComponentOps* Ops = Struct != nullptr ? Struct->GetComponentOps() : nullptr;
            if (Ops == nullptr)
            {
                return false;
            }

            // The entity was given a fresh one while the type was gone, and what the user sees is what stays.
            if (Ops->Has(Registry, Entity))
            {
                LOG_WARN("[ECS] Entity {}: '{}' came back but the entity already has one; the held copy was dropped.", (uint32)Entity, Held.TypeName);
                return true;
            }
            Ops->EmplaceSerialized(Registry, Entity, *Held.Data.MakeReader());
            Held.ApplyEntityRemap(Struct, Ops->Get(Registry, Entity));
            ++Restored;
            return true;
        });

        if (Holding.IsEmpty())
        {
            Registry.Remove<SMissingComponents>(Entity);
        }
        else
        {
            Registry.Get<SMissingComponents>(Entity).Entries = Move(Holding);
        }
        return Restored;
    }

    int32 Restore(ECS::FRegistry& Registry)
    {
        TVector<ECS::FEntity> Entities;
        for (ECS::FEntity Entity : Registry.View<SMissingComponents>())
        {
            Entities.Add(Entity);
        }

        int32 Restored = 0;
        for (ECS::FEntity Entity : Entities)
        {
            Restored += Restore(Registry, Entity);
        }
        if (Restored > 0)
        {
            LOG_INFO("[ECS] Restored {} held component(s) whose types are loaded again.", Restored);
        }
        return Restored;
    }

    int32 HoldOrphaned(ECS::FRegistry& Registry)
    {
        TVector<ECS::FSparseSet*> Orphaned;
        for (ECS::FSparseSet* Set : Registry.GetActiveStorages())
        {
            const ECS::FComponentTypeInfo& Info = Set->GetTypeInfo();
            const CScriptStruct* Script = Info.bRuntimeLayout ? Cast<CScriptStruct>(Info.GetBoundStruct()) : nullptr;
            if (Script != nullptr && Script->IsSuperseded() && Set->Num() > 0)
            {
                Orphaned.Add(Set);
            }
        }

        int32 Held = 0;
        for (ECS::FSparseSet* Set : Orphaned)
        {
            const ECS::FComponentTypeInfo& Info = Set->GetTypeInfo();
            const CStruct& Struct = *Info.GetBoundStruct();
            const FName TypeName = StructIdentity(&Struct);
            const ECS::FComponentTypeID TypeID = Info.TypeID;

            TVector<ECS::FEntity> Entities;
            for (ECS::FEntity Entity : *Set)
            {
                Entities.Add(Entity);
            }
            for (ECS::FEntity Entity : Entities)
            {
                SHeldValue Value;
                Value.TypeName = TypeName;
                Value.Data.Capture(Struct, Set->GetRaw(Entity));
                Registry.RemoveRaw(TypeID, Entity);
                Hold(Registry, Entity, Move(Value));
                ++Held;
            }
            LOG_WARN("[ECS] '{}' no longer exists, so {} live component(s) are held until it comes back.", TypeName, Entities.Num());
        }
        return Held;
    }
}
