#pragma once

#include "imgui.h"
#include "Core/Reflection/PropertyCustomization/PropertyCustomization.h"
#include "GameplayTags/GameplayTag.h"
#include "Tools/UI/ImGui/Widgets/TreeListView.h"

namespace Lumina
{
    // Draws an FGameplayTag property as a combo that opens an organized, searchable FTreeListView of the
    // registered tags (e.g. Ability > Fire > Fireball), with a "+" button to author a new tag (interned +
    // persisted to CGameplayTagsSettings). FGameplayTagContainer reuses this automatically: its Tags array
    // renders each element with this picker.
    class FGameplayTagPropertyCustomization : public IPropertyTypeCustomization
    {
    public:

        static TSharedPtr<FGameplayTagPropertyCustomization> MakeInstance()
        {
            return MakeShared<FGameplayTagPropertyCustomization>();
        }

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

    private:

        // Populates TagTree from the registry, splitting dotted tags into a hierarchy. Each node stores its
        // full dotted path as user data so selection can read it back.
        void BuildTagTree(FTreeListView& Tree);

        FGameplayTag    Value;
        FTreeListView   TagTree;
        ImGuiTextFilter TagFilter;
        int32           LastBuiltCount = 0;
        char            NewTagBuffer[128] = {};
    };
}
