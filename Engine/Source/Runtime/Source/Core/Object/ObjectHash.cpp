#include "RuntimePCH.h"
#include "ObjectHash.h"

#include "ObjectBase.h"
#include "Package/Package.h"

namespace Lumina
{
    void FObjectHashTables::AddObject(CObjectBase* Object)
    {
        LUMINA_PROFILE_SCOPE();
        FWriteScopeLock Lock(Mutex);

        const FGuid& ObjectGUID = Object->GetGUID();
        auto It = ObjectGUIDHash.find(ObjectGUID);
        ASSERT(It == ObjectGUIDHash.end());

        ObjectGUIDHash.emplace(ObjectGUID, Object);
        ObjectNameHash[Object->GetName()].emplace(Object);
    }

    void FObjectHashTables::RemoveObject(CObjectBase* Object)
    {
        LUMINA_PROFILE_SCOPE();
        FWriteScopeLock Lock(Mutex);

        const FGuid& ObjectGUID = Object->GetGUID();
        auto It = ObjectGUIDHash.find(ObjectGUID);
        ASSERT(It != ObjectGUIDHash.end());

        ObjectGUIDHash.erase(It);

        // Uses the object's CURRENT name -- HandleNameChange removes under the old name before it mutates.
        ObjectNameHash[Object->GetName()].erase(Object);
    }

    CObjectBase* FObjectHashTables::FindObject(const FGuid& GUID)
    {
        LUMINA_PROFILE_SCOPE();
        FReadScopeLock Lock(Mutex);

        auto It = ObjectGUIDHash.find(GUID);
        if (It == ObjectGUIDHash.end())
        {
            return nullptr;
        }
        
        if (It->second->HasAnyFlag(OF_MarkedDestroy))
        {
            return nullptr;
        }

        return It->second;
    }

    CObjectBase* FObjectHashTables::FindObject(const FName& Name, CClass* Class)
    {
        LUMINA_PROFILE_SCOPE();
        FReadScopeLock Lock(Mutex);

        auto It = ObjectNameHash.find(Name);
        if (It == ObjectNameHash.end())
        {
            return nullptr;
        }
        
        for (CObjectBase* Object : It->second)
        {
            if (Object->GetClass() == Class && !Object->HasAnyFlag(OF_MarkedDestroy))
            {
                return Object;
            }
        }

        return nullptr;
    }


    void FObjectHashTables::Clear()
    {
        FWriteScopeLock Lock(Mutex);
        ObjectGUIDHash.clear();
        ObjectNameHash.clear();
    }
}
