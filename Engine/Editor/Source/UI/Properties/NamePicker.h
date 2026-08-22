#pragma once

#include "Containers/Name.h"
#include "Containers/Vector.h"
#include "Core/Reflection/PropertyCustomization/PropertyEditContext.h"
#include "Memory/SmartPtr.h"
#include "Tools/UI/ImGui/Widgets/TreeListView.h"
#include "imgui.h"

namespace Lumina
{
    // Widget state for one picker instance, owned by the property row, never by the shared source.
    struct FNamePickerState
    {
        ImGuiTextFilter Filter;
        FTreeListView   Tree;
        int32           LastBuiltCount = 0;
    };

    struct FNamePickerArgs
    {
        const char*                 StrId = nullptr;
        const FPropertyEditContext& Context = FPropertyEditContext::None();
        FName                       Current;
        FNamePickerState*           State = nullptr;
    };

    struct FNamePickerResult
    {
        bool  bChanged = false;
        FName Value;
    };

    // One picker kind, registered by the module that owns the domain rather than by the customization.
    struct INamePickerSource
    {
        virtual ~INamePickerSource() = default;

        virtual void GatherChoices(const FPropertyEditContext& Context, TVector<FName>& Out) const = 0;

        virtual const char* GetItemIcon() const { return nullptr; }

        // True lets the search text be committed as a new name, for a list the edit itself extends.
        virtual bool AllowsCustomNames() const { return false; }

        // Shown when the stored name is not among the gathered choices.
        virtual const char* GetStaleHint() const { return nullptr; }

        // Shown when nothing was gathered, so a missing provider never reads as an empty list.
        virtual const char* GetUnavailableHint(const FPropertyEditContext& Context) const { return nullptr; }

        // A hierarchical picker draws its own body and ignores GatherChoices.
        virtual bool WantsCustomBody() const { return false; }
        virtual FNamePickerResult DrawCustomBody(const FNamePickerArgs& Args) const { return {}; }
    };

    namespace NamePicker
    {
        EDITOR_API void Register(const FName& Kind, TSharedPtr<INamePickerSource> Source);

        EDITOR_API const INamePickerSource* Find(const FName& Kind);

        // The shared combo for a list source, or the source's own body when it wants one.
        EDITOR_API FNamePickerResult Draw(const INamePickerSource& Source, const FNamePickerArgs& Args);

        void RegisterBuiltInSources();
    }
}
