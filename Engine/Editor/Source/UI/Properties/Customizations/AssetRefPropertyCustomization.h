#pragma once

#include "ImGuiDrawUtils.h"
#include "Core/Reflection/PropertyCustomization/PropertyCustomization.h"
#include "Assets/AssetRef.h"

namespace Lumina
{
    // Generic editor picker for FAssetRef slots. Reads the property's AssetType meta (e.g. "luau", "rml")
    // to filter the candidate list + drag-drop, and writes back both the path and the stable GUID so the
    // reference survives later renames. Registered against FAssetRef::StaticStruct()->GetName().
    class FAssetRefPropertyCustomization : public IPropertyTypeCustomization
    {
    public:

        static TSharedPtr<FAssetRefPropertyCustomization> MakeInstance();

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

        ImGuiTextFilter SearchFilter;

    private:

        // Deferred so the undo snapshot (taken in UpdatePropertyValue, after BeginTransaction) is pre-change.
        TFunction<void()> PendingMutation;
        bool bFinishPending = false;
    };
}
