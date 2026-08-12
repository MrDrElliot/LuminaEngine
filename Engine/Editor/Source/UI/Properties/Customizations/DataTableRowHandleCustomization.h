#pragma once

#include "Assets/AssetTypes/DataTable/DataTable.h"
#include "Core/Reflection/PropertyCustomization/PropertyCustomization.h"
#include "imgui.h"

namespace Lumina
{
    // Table picker, then a row name read off that table.
    class FDataTableRowHandlePropertyCustomization : public IPropertyTypeCustomization
    {
    public:

        static TSharedPtr<FDataTableRowHandlePropertyCustomization> MakeInstance();

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

    private:

        SDataTableRowHandle CachedValue;
        SDataTableRowHandle DisplayValue;

        ImGuiTextFilter TableFilter;

        // Picks are one-frame edits: Started on the change frame, Finished the next, so they pair
        // into one undo transaction.
        bool bFinishPending = false;
    };
}
