#pragma once

#include "imgui.h"
#include "Containers/Function.h"
#include "Core/Reflection/PropertyCustomization/PropertyCustomization.h"
#include "Assets/AssetManager/AssetManager.h"

namespace Lumina
{
    // Picker for SAssetId, listing every REFLECT(Group = ...) asset under its group, or only one group when the field names it.
    class FAssetIdPropertyCustomization : public IPropertyTypeCustomization
    {
    public:

        static TSharedPtr<FAssetIdPropertyCustomization> MakeInstance();

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override {}

    private:

        ImGuiTextFilter SearchFilter;

        // Rebuilt once on popup open rather than every frame it stays open.
        TVector<FName> CachedGroups;
        TVector<TVector<SAssetId>> CachedGroupMembers; // parallel to CachedGroups

        // Applied in UpdatePropertyValue, after the undo snapshot was taken, so the snapshot holds the old id.
        TFunction<void()> PendingMutation;
        bool bFinishPending = false;
    };
}
