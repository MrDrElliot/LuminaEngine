#include "RuntimePCH.h"
#ifdef LE_PLATFORM_LINUX

#include "Platform/Display/PlatformDisplay.h"

namespace Lumina::PlatformDisplay
{
    // No compositor here reports an HDR mode yet, so every monitor reads as SDR.
    FHDRDisplayInfo QueryHDRDisplay(int32, int32)
    {
        return {};
    }
}

#endif
