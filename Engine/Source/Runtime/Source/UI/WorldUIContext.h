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

        // Rotates which widget starts this world's render budget each frame, independent of other worlds.
        uint32                                   WidgetRenderCursor = 0;

        TVector<UIScripts::FOpenUI>              OpenScriptUIs;

        // A frame with no input, no edit and nothing animating skips RmlUi's update and render walks and replays the last batch.
        uint64                                   ChangeGeneration = 1;
        uint64                                   SeenChangeGeneration = 0;
        double                                   NextUpdateSeconds = 0.0;
        FUIntVector2                             LastLayoutSize{0, 0};
        float                                    LastDpRatio = 0.0f;
        bool                                     bIdleThisFrame = false;

        // Returns the job this frame's render budget starts at and rotates the start for the next frame.
        uint32 NextWidgetRenderStart(uint32 JobCount)
        {
            const uint32 Start = WidgetRenderCursor % JobCount;
            WidgetRenderCursor = (Start + 1) % JobCount;
            return Start;
        }

        // Sets bIdleThisFrame and, when the frame needs RmlUi's update, records the state that update is about to see.
        bool NeedsUpdate(const FUIntVector2& LayoutSize, float DpRatio, double Now)
        {
            bIdleThisFrame = SeenChangeGeneration == ChangeGeneration && LayoutSize == LastLayoutSize
                && DpRatio == LastDpRatio && Now < NextUpdateSeconds;
            if (bIdleThisFrame)
            {
                return false;
            }
            SeenChangeGeneration = ChangeGeneration;
            LastLayoutSize = LayoutSize;
            LastDpRatio = DpRatio;
            return true;
        }
    };
}
