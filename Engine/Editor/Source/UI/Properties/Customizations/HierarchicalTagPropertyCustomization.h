#pragma once

#include "Core/Reflection/PropertyCustomization/PropertyCustomization.h"
#include "HierarchicalTags/HierarchicalTag.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    class FHierarchicalTagPicker;

    // One tag as a field opening a searchable tag tree, which is also where new tags are created.
    class FHierarchicalTagPropertyCustomization : public IPropertyTypeCustomization
    {
    public:

        FHierarchicalTagPropertyCustomization();
        ~FHierarchicalTagPropertyCustomization() override;

        static TSharedPtr<FHierarchicalTagPropertyCustomization> MakeInstance()
        {
            return MakeShared<FHierarchicalTagPropertyCustomization>();
        }

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

    private:

        SHierarchicalTag                  Value;
        FPropertyEditSession              EditSession;
        TUniquePtr<FHierarchicalTagPicker> Picker;
    };

    // A tag container as a vertical list under an Edit Tags button, whose tree ticks tags in and out.
    class FHierarchicalTagContainerPropertyCustomization : public IPropertyTypeCustomization
    {
    public:

        FHierarchicalTagContainerPropertyCustomization();
        ~FHierarchicalTagContainerPropertyCustomization() override;

        static TSharedPtr<FHierarchicalTagContainerPropertyCustomization> MakeInstance()
        {
            return MakeShared<FHierarchicalTagContainerPropertyCustomization>();
        }

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

    private:

        SHierarchicalTagContainer          Value;
        FPropertyEditSession               EditSession;
        TUniquePtr<FHierarchicalTagPicker> Picker;
    };
}
