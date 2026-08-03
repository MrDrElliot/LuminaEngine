#include "Networking/INetworkRuntime.h"

namespace Lumina
{
    namespace
    {
        // Null until something installs one. The engine treats that as "no networking", not an error.
        INetworkRuntime* GNetworkRuntime = nullptr;
    }

    void SetNetworkRuntime(INetworkRuntime* Runtime)
    {
        GNetworkRuntime = Runtime;
    }

    INetworkRuntime* GetNetworkRuntime()
    {
        return GNetworkRuntime;
    }
}
