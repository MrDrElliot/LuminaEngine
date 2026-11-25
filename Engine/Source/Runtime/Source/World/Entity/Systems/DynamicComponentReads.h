#pragma once

#include <atomic>

#include "SystemParam.h"
#include "Containers/Vector.h"
#include "Core/Threading/Mutex.h"
#include "World/ECS/Entity.h"

namespace Lumina
{
    // A system member reading components whose types only data names, such as an asset, declared to the scheduler like a query's reads.
    class RUNTIME_API FDynamicComponentReads
    {
    public:

        FDynamicComponentReads();

        // True once the type is declared, from which point no system writing it runs alongside this one.
        NODISCARD bool Declares(uint32 TypeId) const;

        // The entity's component of a declared type, or null when it has none or the type is not declared yet.
        NODISCARD const void* Find(uint32 TypeId, ECS::FEntity Entity) const;

        // Declares the type before the next update stage, and until then reads of it find nothing. Safe from any thread.
        void Request(uint32 TypeId);

    private:

        static const FSystemParamOps& GetOps();

        ECS::FRegistry* Registry = nullptr;
        CWorld*         World    = nullptr;

        // Sorted, and changed only while the world reconfigures between update stages, so workers read it unlocked.
        TVector<uint32> Declared;

        FMutex            RequestMutex;
        TVector<uint32>   Requested;
        std::atomic<bool> bHasRequests{ false };
    };
}
