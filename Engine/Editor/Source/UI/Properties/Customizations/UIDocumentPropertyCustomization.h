#pragma once

#include "imgui.h"
#include "Containers/Function.h"
#include "Core/Reflection/PropertyCustomization/PropertyCustomization.h"
#include "Assets/AssetRegistry/AssetRegistry.h"

namespace Lumina
{
    // Picker for SUIDocumentRef slots, listing the project's .rml documents by name the way an asset slot lists assets.
    class FUIDocumentPropertyCustomization : public IPropertyTypeCustomization
    {
    public:

        static TSharedPtr<FUIDocumentPropertyCustomization> MakeInstance();

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override {}

    private:

        ImGuiTextFilter SearchFilter;

        // Rebuilt once on popup open rather than every frame it stays open.
        TVector<FTextAssetDataPtr> CachedDocuments;

        // Applied in UpdatePropertyValue, after the undo snapshot was taken, so the snapshot holds the old document.
        TFunction<void()> PendingMutation;
        bool bFinishPending = false;
    };
}
