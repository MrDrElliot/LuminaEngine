#include "RuntimePCH.h"
#include "EngineSettings.h"

#include "Renderer/RenderManager.h"
#include "Renderer/RHI.h"

namespace Lumina
{
    void CRendererSettings::PostInitSettings()
    {
        Super::PostInitSettings();
        ApplyPresentMode();
    }

    void CRendererSettings::ApplyPresentMode() const
    {
        if (RHI::GetPresentMode() == PresentMode)
        {
            return;
        }

        RHI::SetPresentMode(PresentMode);

        if (FRenderManager* Manager = TryRender())
        {
            Manager->RecreatePrimarySwapchain();
        }
    }
}
