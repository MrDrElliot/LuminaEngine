#pragma once

#include "Core/Math/Math.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Renderer/RHI.h"

namespace Rml
{
    class Context;
    class ElementDocument;
}

namespace Lumina
{
    class CEntityScript;
    class CWorld;

    namespace UIScripts
    {
        // A CUIScript's document and data model, held by the world the script lives in.
        struct FOpenUI
        {
            CEntityScript* Script = nullptr;
            CWorld*        World = nullptr;
            void*          Model = nullptr;
            void*          Document = nullptr;
            FString        Path;
            FString        ModelName;
            // What the script asked for, kept across a reload that fails, so fixing the file brings the UI back.
            bool           bWantShown = false;
            // The input mode lease this UI holds, so it is released even if Interactive changed since.
            uint64         CursorLease = 0;
            int32          Generation = -1;
        };
    }

    // One world-space widget to rasterize this frame; built in Tick, consumed in Render.
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

        TVector<UIScripts::FOpenUI>              OpenScriptUIs;

        // A frame with no input, no edit and nothing animating skips RmlUi's update and render walks and replays the last batch.
        uint64                                   SeenChangeGeneration = 0;
        double                                   NextUpdateSeconds = 0.0;
        FUIntVector2                             LastLayoutSize{0, 0};
        float                                    LastDpRatio = 0.0f;
        bool                                     bIdleThisFrame = false;
    };
}
