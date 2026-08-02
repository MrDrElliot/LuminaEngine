#pragma once

#include "Core/Math/Math.h"
#include "Containers/String.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Templates/LuminaTemplate.h"
#include "Assets/AssetRef.h"
#include "Renderer/RHITexture.h"
#include "WidgetComponent.generated.h"

namespace Rml
{
    class Context;
    class ElementDocument;
}

namespace Lumina
{
    // Per-instance live state for a world widget (its Rml context + offscreen RT). Not serialized; rebuilt
    // on demand. Copies start EMPTY so a duplicate doesn't alias the source; moves transfer ownership.
    struct FWidgetRuntime
    {
        Rml::Context*         Context = nullptr;
        Rml::ElementDocument* Document = nullptr;
        RHI::FManagedTexture  Target;
        FUIntVector2          BuiltSize{0, 0};
        FString               LoadedPath;
        int64                 DocWriteTime = 0;   // last-seen .rml mtime, for hot-reload
        int32                 ResourceID   = -1;  // global-heap id of Target, read by the gather
        bool                  bVisible     = true;// set by the render gather (frustum); gates Update/rasterize
        bool                  bRmlIdle     = false;// last Update reported no pending animation/transition (GetNextUpdateDelay infinite)

        FWidgetRuntime() = default;
        ~FWidgetRuntime() = default;

        FWidgetRuntime(const FWidgetRuntime&) {}
        FWidgetRuntime& operator=(const FWidgetRuntime&) { return *this; }

        FWidgetRuntime(FWidgetRuntime&& Other) noexcept { *this = Move(Other); }
        FWidgetRuntime& operator=(FWidgetRuntime&& Other) noexcept
        {
            Context      = Other.Context;
            Document     = Other.Document;
            Target       = Other.Target;
            BuiltSize    = Other.BuiltSize;
            LoadedPath   = Move(Other.LoadedPath);
            DocWriteTime = Other.DocWriteTime;
            ResourceID   = Other.ResourceID;
            bVisible     = Other.bVisible;
            bRmlIdle     = Other.bRmlIdle;
            Other.Context    = nullptr;
            Other.Document   = nullptr;
            Other.Target     = {};
            Other.ResourceID = -1;
            return *this;
        }
    };

    // Renders an RmlUi document onto a world-space quad. Laid out + rasterized into Runtime.Target each
    // frame; the widget pass draws a textured quad sampling that RT bindlessly.
    REFLECT(Component, Category = "UI")
    struct RUNTIME_API SWidgetComponent
    {
        GENERATED_BODY()

        /** RML document to display, e.g. "/Game/UI/MyWidget.rml". Empty = nothing drawn. Rename-safe. */
        PROPERTY(Editable, Category = "Widget", AssetType = "rml")
        FAssetRef DocumentPath;

        /** Offscreen render-target resolution the document is laid out at (pixels). */
        PROPERTY(Editable, Category = "Widget")
        int32 DrawWidth = 512;

        PROPERTY(Editable, Category = "Widget")
        int32 DrawHeight = 512;

        /** Physical quad size in world units. */
        PROPERTY(Editable, Category = "Widget")
        FVector2 WorldSize = FVector2(1.0f, 1.0f);

        /** When true the quad always faces the camera; otherwise it uses the entity's orientation. */
        PROPERTY(Editable, Category = "Widget")
        bool bBillboard = true;

        /** RGBA tint multiplied with the widget color. */
        PROPERTY(Editable, Color, Category = "Widget")
        FVector4 Tint = FVector4(1.0f);

        FWidgetRuntime Runtime;
    };
}
