#include "RuntimePCH.h"
#include "DynamicComponentReads.h"

#include "Containers/Algorithm.h"
#include "World/ECS/Registry.h"

namespace Lumina
{
    FDynamicComponentReads::FDynamicComponentReads()
    {
        RegisterSystemParam(this, GetOps());
    }

    bool FDynamicComponentReads::Declares(uint32 TypeId) const
    {
        return Algo::BinarySearch(Declared, TypeId);
    }

    const void* FDynamicComponentReads::Find(uint32 TypeId, ECS::FEntity Entity) const
    {
        if (Registry == nullptr || !Declares(TypeId))
        {
            return nullptr;
        }
        const ECS::FSparseSet* Storage = Registry->FindStorage(TypeId);
        return Storage != nullptr ? Storage->GetRaw(Entity) : nullptr;
    }

    void FDynamicComponentReads::Request(uint32 TypeId)
    {
        if (World == nullptr || Declares(TypeId))
        {
            return;
        }

        {
            FScopeLock Lock(RequestMutex);
            if (Algo::Contains(Requested, TypeId))
            {
                return;
            }
            Requested.Add(TypeId);
        }
        bHasRequests.store(true, std::memory_order_release);
        RequestSystemRedeclare(*World);
    }

    const FSystemParamOps& FDynamicComponentReads::GetOps()
    {
        static const FSystemParamOps Ops
        {
            // Runs while the world reconfigures the system, so the requests join the declared set before any worker reads it.
            .Bind = [](void* Param, CWorld& InWorld)
            {
                FDynamicComponentReads& Reads = *static_cast<FDynamicComponentReads*>(Param);
                Reads.Registry = &GetSystemRegistry(InWorld);
                Reads.World    = &InWorld;

                FScopeLock Lock(Reads.RequestMutex);
                for (const uint32 TypeId : Reads.Requested)
                {
                    Reads.Declared.AddUnique(TypeId);
                }
                Reads.Requested.Reset();
                Algo::Sort(Reads.Declared);
                Reads.bHasRequests.store(false, std::memory_order_release);
            },
            .Declare = [](const void* Param, FSystemAccess& Access)
            {
                for (const uint32 TypeId : static_cast<const FDynamicComponentReads*>(Param)->Declared)
                {
                    DeclareParamRead(Access, TypeId, true);
                }
            },
            .Unbind = [](void* Param)
            {
                FDynamicComponentReads& Reads = *static_cast<FDynamicComponentReads*>(Param);
                Reads.Registry = nullptr;
                Reads.World    = nullptr;
            },
            .NeedsRedeclare = [](const void* Param) -> bool
            {
                return static_cast<const FDynamicComponentReads*>(Param)->bHasRequests.load(std::memory_order_acquire);
            },
        };
        return Ops;
    }
}
