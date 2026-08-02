#include "RuntimePCH.h"
#include "DevelopmentToolUI.h"

#include "Core/Profiler/Profile.h"


namespace Lumina
{
    void IDevelopmentToolUI::Update(const FUpdateContext& UpdateContext)
    {
        OnUpdate(UpdateContext);
    }

    void IDevelopmentToolUI::StartFrame(const FUpdateContext& UpdateContext)
    {
        OnStartFrame(UpdateContext);
    }

    void IDevelopmentToolUI::EndFrame(const FUpdateContext& UpdateContext)
    {
        OnEndFrame(UpdateContext);
    }
}
