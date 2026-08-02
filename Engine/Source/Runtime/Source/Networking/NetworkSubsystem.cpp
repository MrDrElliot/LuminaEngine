#include "RuntimePCH.h"
#include <enet/enet.h>
#include "NetworkGlobals.h"
#include "INetworkTransport.h"
#include "ENet/ENetTransport.h"
#include "Memory/Memory.h"
#include "Memory/MemoryTracking.h"
#include "Log/Log.h"

namespace Lumina
{
    INetworkTransport* GNetwork = nullptr;

    namespace
    {
        TVector<FNetworkEvent> GFrameEvents;

        void* ENetMalloc(size_t Size)
        {
            LUMINA_MEMORY_SCOPE("Network");
            return Memory::Malloc(Size);
        }

        void ENetFree(void* Ptr)
        {
            Memory::Free(Ptr); // Memory::Free takes void*&; Ptr is a lvalue, so this binds.
        }

        void ENetNoMemory()
        {
            LOG_ERROR("Network: ENet reported out of memory");
        }
    }

    namespace Network
    {
        void Initialize()
        {
            ENetCallbacks Callbacks{};
            Callbacks.malloc    = &ENetMalloc;
            Callbacks.free      = &ENetFree;
            Callbacks.no_memory = &ENetNoMemory;

            if (enet_initialize_with_callbacks(ENET_VERSION, &Callbacks) != 0)
            {
                LOG_ERROR("Network: enet_initialize_with_callbacks failed");
                return;
            }

            GNetwork = new FENetTransport();
            LOG_DISPLAY("Network: initialized (ENet backend)");
        }

        void Shutdown()
        {
            delete GNetwork;
            GNetwork = nullptr;

            GFrameEvents.clear();
            GFrameEvents.shrink_to_fit();

            enet_deinitialize();
        }

        void Update()
        {
            GFrameEvents.clear();

            if (GNetwork != nullptr)
            {
                GNetwork->Service(GFrameEvents);
            }
        }

        const TVector<FNetworkEvent>& GetFrameEvents()
        {
            return GFrameEvents;
        }

        INetworkTransport* CreateTransport()
        {
            return new FENetTransport();
        }
    }
}
