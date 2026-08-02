#pragma once

#include "Core/Math/Math.h"
#include "Containers/Array.h"
#include "Containers/String.h"
#include "Renderer/RHI.h"

namespace Rml
{
    class Context;
    class ElementDocument;
}

namespace Lumina
{
    // One world-space widget to rasterize this frame; built game-thread in Tick, consumed render-thread in Render.
    // Non-owning: the Rml context + RT live on the SWidgetComponent that outlives the frame.
    struct FWidgetRenderJob
    {
        Rml::Context*  Context = nullptr;
        RHI::FTextureH Target = {};
        FUIntVector2     Size{0, 0};
    };

    // Per-world UI state owned by CWorld: the world's own Rml context + loaded documents (bridge holds only
    // process-global state). Lifetime tracks the world via Initialize/TeardownWorld.
    struct FWorldUIContext
    {
        Rml::Context*                            Context = nullptr;
        THashMap<FString, Rml::ElementDocument*> Documents;

        // Editor override: lay UI out at this size instead of the RT image size.
        // Zero means use the RT size (standalone-runtime default).
        FUIntVector2                               DisplaySize{0, 0};

        // Per-frame world-space widget render jobs (cleared + refilled each TickWorldWidgets).
        TVector<FWidgetRenderJob>                WidgetJobs;
    };
}
