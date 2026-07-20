#include "pch.h"
#include "RenderSceneFactory.h"

#include "Forward/ForwardRenderScene.h"
#include "Log/Log.h"

namespace Lumina::RenderSceneFactory
{
    static FCreateFn GOverrideFactory = nullptr;

    void SetOverride(FCreateFn Factory, const char* DebugName)
    {
        GOverrideFactory = Factory;

        if (Factory != nullptr)
        {
            LOG_INFO("RenderSceneFactory: renderer override installed ({})", DebugName ? DebugName : "unnamed");
        }
        else
        {
            LOG_INFO("RenderSceneFactory: renderer override cleared, using engine default");
        }
    }

    bool HasOverride()
    {
        return GOverrideFactory != nullptr;
    }

    FCreateFn GetOverride()
    {
        return GOverrideFactory;
    }

    TUniquePtr<IRenderScene> Create(CWorld* World)
    {
        // A null return is the override declining this world (e.g. editor/utility worlds); fall back.
        if (GOverrideFactory != nullptr)
        {
            if (TUniquePtr<IRenderScene> Scene = GOverrideFactory(World))
            {
                return Scene;
            }
        }

        return MakeUnique<FForwardRenderScene>(World);
    }
}
