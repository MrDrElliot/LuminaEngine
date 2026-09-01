#pragma once

#include "RHI.h"

namespace Lumina::RHI::Internal
{
    void DestroyNow(const FGPUAllocation& Allocation);
    void DestroyNow(FTextureH Texture);
    void DestroyNow(FPipelineH Pipeline);
}
