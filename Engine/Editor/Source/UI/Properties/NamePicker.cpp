#include "EditorPCH.h"
#include "NamePicker.h"

#include "Containers/HashTable.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina::NamePicker
{
    namespace
    {
        THashMap<FName, TSharedPtr<INamePickerSource>>& GetSources()
        {
            static THashMap<FName, TSharedPtr<INamePickerSource>> Sources;
            return Sources;
        }
    }

    void Register(const FName& Kind, TSharedPtr<INamePickerSource> Source)
    {
        GetSources()[Kind] = Source;
    }

    const INamePickerSource* Find(const FName& Kind)
    {
        const auto It = GetSources().find(Kind);
        return (It != GetSources().end()) ? It->second.get() : nullptr;
    }

    FNamePickerResult Draw(const INamePickerSource& Source, const FNamePickerArgs& Args)
    {
        if (Source.WantsCustomBody())
        {
            return Source.DrawCustomBody(Args);
        }

        TVector<FName> Choices;
        Source.GatherChoices(Args.Context, Choices);

        // "None" occupies row 0, so every choice sits one slot further along.
        constexpr int32 NoneOffset = 1;

        int32 CurrentIndex = Args.Current.IsNone() ? 0 : INDEX_NONE;
        for (int32 i = 0; i < (int32)Choices.size(); ++i)
        {
            if (Choices[i] == Args.Current)
            {
                CurrentIndex = i + NoneOffset;
                break;
            }
        }

        // A name the provider no longer offers still shows, flagged, rather than reading as None.
        const bool bStale = CurrentIndex == INDEX_NONE;
        FFixedString Preview;
        if (bStale)
        {
            Preview = LE_ICON_ALERT_CIRCLE_OUTLINE "  ";
        }
        Preview += Args.Current.IsNone() ? "None" : Args.Current.c_str();

        FFixedString Created;

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        const int32 Picked = ImGuiX::SearchableCombo(Args.StrId, Preview.c_str(), (int32)Choices.size() + NoneOffset,
            CurrentIndex, [&Choices](int32 Index)
            {
                return (Index < NoneOffset) ? FFixedString("None") : FFixedString(Choices[Index - NoneOffset].c_str());
            }, Source.GetItemIcon(), Source.AllowsCustomNames() ? &Created : nullptr);

        const char* Hint = bStale ? Source.GetStaleHint()
                                  : (Choices.empty() ? Source.GetUnavailableHint(Args.Context) : nullptr);
        if (Hint != nullptr)
        {
            ImGuiX::TextTooltip_Internal(Hint);
        }

        if (!Created.empty())
        {
            return { true, FName(Created.c_str()) };
        }

        if (Picked != INDEX_NONE && Picked != CurrentIndex)
        {
            return { true, (Picked < NoneOffset) ? FName() : Choices[Picked - NoneOffset] };
        }

        return {};
    }
}
