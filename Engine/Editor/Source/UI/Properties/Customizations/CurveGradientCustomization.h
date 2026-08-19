#pragma once

#include "imgui.h"
#include "Assets/AssetTypes/Curve/CurveAsset.h"
#include "Assets/AssetTypes/Curve/Gradient.h"
#include "Core/Reflection/PropertyCustomization/PropertyCustomization.h"
#include "UI/CurveEditor/CurveEditorWidget.h"

namespace Lumina
{
    /** Draws an SCurve row as a live thumbnail of the curve plus a source toggle, opening the full
     *  FCurveEditorWidget in a popup. The widget is the same one the curve asset editor uses, so an inline
     *  curve is authored with exactly the tools a shared one is.
     *
     *  In asset mode the thumbnail shows the asset's shape and the popup is read-only: the curve belongs to
     *  another asset, and editing it from here would silently change every other user of it. */
    class FCurvePropertyCustomization : public IPropertyTypeCustomization
    {
    public:

        static TSharedPtr<FCurvePropertyCustomization> MakeInstance()
        {
            return MakeShared<FCurvePropertyCustomization>();
        }

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

    private:

        /** Samples the resolved curve across its own time range into the given rect. */
        void DrawThumbnail(const SKeyedCurve& InCurve, const ImVec2& Size) const;

        /** Delegates the Asset member to the standard object picker so it behaves exactly like every other
         *  asset slot in the editor -- browse popup, thumbnail, type filtering, drop target, clear -- rather
         *  than a bespoke half-implementation of the same thing. Returns true if the reference changed. */
        bool DrawAssetSlot();

        SCurve              Value;
        SCurve              CachedValue;
        FCurveEditorWidget  Editor;

        /** The real picker, driven through a handle synthesized over this customization's own copy of the
         *  struct (the table only hands us a handle for the struct as a whole). */
        TSharedPtr<IPropertyTypeCustomization> AssetPicker;
        TSharedPtr<FPropertyHandle>            AssetHandle;

        // The widget writes through a pointer, so it must point at the copy this customization owns for
        // the lifetime of the popup; rebinding every frame keeps it valid across a HandleExternalUpdate
        // that replaces Value.
        bool                bEditorOpen = false;
        bool                bDirty      = false;
    };

    /** Draws an SGradient row as the ramp itself, opening a stop editor in a popup: click a stop to select,
     *  drag to move it, double-click the bar to insert, and edit the selected color with the standard
     *  picker. Stops stay time-sorted, which SGradient::Evaluate relies on. */
    class FGradientPropertyCustomization : public IPropertyTypeCustomization
    {
    public:

        static TSharedPtr<FGradientPropertyCustomization> MakeInstance()
        {
            return MakeShared<FGradientPropertyCustomization>();
        }

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

    private:

        /** Ramp preview. Draws a checkerboard underneath so alpha reads as transparency rather than black. */
        void DrawRamp(const ImVec2& Min, const ImVec2& Max) const;

        /** Stop handles under the ramp; returns true when a stop moved, was added, or was removed. */
        bool DrawStops(const ImVec2& RampMin, const ImVec2& RampMax);

        SGradient   Value;
        SGradient   CachedValue;
        int32       SelectedStop = INDEX_NONE;
        bool        bDirty       = false;
    };
}
