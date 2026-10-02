#include "RuntimePCH.h"
#include "ObjectHash.h"

#include "ObjectBase.h"
#include "Package/Package.h"

namespace Lumina
{
    void FObjectHashTables::AddObject(CObjectBase* Object)
    {
        LUMINA_PROFILE_SCOPE();

        const FGuid& ObjectGUID = Object->GetGUID();
        {
            FGuidShard& Shard = GuidShards[ShardOf(ObjectGUID)];
            FWriteScopeLock Lock(Shard.Mutex);
            ASSERT(Shard.Objects.find(ObjectGUID) == Shard.Objects.end());
            Shard.Objects.emplace(ObjectGUID, Object);
        }
        {
            const FName Name = Object->GetName();
            FNameShard& Shard = NameShards[ShardOf(Name)];
            FWriteScopeLock Lock(Shard.Mutex);
            Shard.Objects[Name].emplace(Object);
        }
    }

    void FObjectHashTables::RemoveObject(CObjectBase* Object)
    {
        LUMINA_PROFILE_SCOPE();

        const FGuid& ObjectGUID = Object->GetGUID();
        {
            FGuidShard& Shard = GuidShards[ShardOf(ObjectGUID)];
            FWriteScopeLock Lock(Shard.Mutex);
            auto It = Shard.Objects.find(ObjectGUID);
            ASSERT(It != Shard.Objects.end());
            Shard.Objects.erase(It);
        }
        {
            // Uses the object's current name, since HandleNameChange removes before it mutates.
            const FName Name = Object->GetName();
            FNameShard& Shard = NameShards[ShardOf(Name)];
            FWriteScopeLock Lock(Shard.Mutex);
            auto It = Shard.Objects.find(Name);
            if (It != Shard.Objects.end())
            {
                It->second.erase(Object);
                if (It->second.empty())
                {
                    Shard.Objects.erase(It);
                }
            }
        }
    }

    CObjectBase* FObjectHashTables::FindObject(const FGuid& GUID)
    {
        LUMINA_PROFILE_SCOPE();
        const FGuidShard& Shard = GuidShards[ShardOf(GUID)];
        FReadScopeLock Lock(Shard.Mutex);

        auto It = Shard.Objects.find(GUID);
        if (It == Shard.Objects.end() || It->second->HasAnyFlag(OF_MarkedDestroy))
        {
            return nullptr;
        }
        return It->second;
    }

    CObjectBase* FObjectHashTables::FindObject(const FName& Name, CClass* Class)
    {
        LUMINA_PROFILE_SCOPE();
        const FNameShard& Shard = NameShards[ShardOf(Name)];
        FReadScopeLock Lock(Shard.Mutex);

        auto It = Shard.Objects.find(Name);
        if (It == Shard.Objects.end())
        {
            return nullptr;
        }

        for (CObjectBase* Object : It->second)
        {
            // Subclasses count: FindObject<T> asks for a base, and a minted class answers as a CScriptClass
            // rather than a CClass, so an exact compare would stop finding it by name.
            if (Object->GetClass() != nullptr && Object->GetClass()->IsChildOf(Class)
                && !Object->HasAnyFlag(OF_MarkedDestroy))
            {
                return Object;
            }
        }

        return nullptr;
    }

    CObjectBase* FObjectHashTables::FindObject(const FName& Name, const CPackage* Package)
    {
        LUMINA_PROFILE_SCOPE();
        const FNameShard& Shard = NameShards[ShardOf(Name)];
        FReadScopeLock Lock(Shard.Mutex);

        auto It = Shard.Objects.find(Name);
        if (It == Shard.Objects.end())
        {
            return nullptr;
        }

        for (CObjectBase* Object : It->second)
        {
            if (Object->GetPackage() == Package && !Object->HasAnyFlag(OF_MarkedDestroy))
            {
                return Object;
            }
        }

        return nullptr;
    }

    void FObjectHashTables::Clear()
    {
        for (FGuidShard& Shard : GuidShards)
        {
            FWriteScopeLock Lock(Shard.Mutex);
            Shard.Objects.clear();
        }
        for (FNameShard& Shard : NameShards)
        {
            FWriteScopeLock Lock(Shard.Mutex);
            Shard.Objects.clear();
        }
    }
}
