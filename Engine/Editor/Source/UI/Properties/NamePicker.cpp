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
        const auto It = GetSources().Find(Kind);
        return (It != nullptr) ? It->get() : nullptr;
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

        const int32 Found = Algo::IndexOf(Choices, Args.Current);
        const int32 CurrentIndex = Found != Constants::kIndexNone ? Found + NoneOffset
                                 : (Args.Current.IsNone() ? 0 : Constants::kIndexNone);

        // A name the provider no longer offers still shows, flagged, rather than reading as None.
        const bool bStale = CurrentIndex == Constants::kIndexNone;
        FFixedString Preview;
        if (bStale)
        {
            Preview = LE_ICON_ALERT_CIRCLE_OUTLINE "  ";
        }
        Preview += Args.Current.IsNone() ? "None" : *Args.Current;

        FFixedString Created;

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        const int32 Picked = ImGuiX::SearchableCombo(Args.StrId, *Preview, Choices.Num() + NoneOffset,
            CurrentIndex, [&Choices](int32 Index)
            {
                return (Index < NoneOffset) ? FFixedString("None") : FFixedString(*Choices[Index - NoneOffset]);
            }, Source.GetItemIcon(), Source.AllowsCustomNames() ? &Created : nullptr);

        const char* Hint = bStale ? Source.GetStaleHint()
                                  : (Choices.IsEmpty() ? Source.GetUnavailableHint(Args.Context) : nullptr);
        if (Hint != nullptr)
        {
            ImGuiX::TextTooltip_Internal(Hint);
        }

        if (!Created.IsEmpty())
        {
            return { true, FName(*Created) };
        }

        if (Picked != Constants::kIndexNone && Picked != CurrentIndex)
        {
            return { true, (Picked < NoneOffset) ? FName() : Choices[Picked - NoneOffset] };
        }

        return {};
    }
}
