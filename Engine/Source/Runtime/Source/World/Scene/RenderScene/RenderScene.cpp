#include "RuntimePCH.h"
#include "RenderScene.h"

#include "World/World.h"

namespace Lumina
{
    FSceneRenderSettings& IRenderScene::GetSceneRenderSettings()
    {
        return World->GetOrEmplaceSingleton<FSceneRenderSettings>();
    }

    void IRenderScene::RefreshFrameSettings()
    {
        FrameSettings = GetSceneRenderSettings();
    }
}
