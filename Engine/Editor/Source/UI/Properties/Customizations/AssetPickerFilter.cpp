#include "EditorPCH.h"
#include "AssetPickerFilter.h"

#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Containers/Algorithm.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Session/SessionOps.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Tools/EditorToolContext.h"

namespace Lumina::AssetPickerFilter
{
    namespace
    {
        // The boundary test stops a plugin path like /GameplayAbilities from reading as /Game content.
        bool HasRoot(FStringView Path, FStringView Root)
        {
            if (Path.Len() < Root.Len())
            {
                return false;
            }

            for (int32 i = 0; i < Root.Len(); ++i)
            {
                char A = Path[i];
                char B = Root[i];
                if (A >= 'A' && A <= 'Z') { A += 32; }
                if (B >= 'A' && B <= 'Z') { B += 32; }
                if (A != B)
                {
                    return false;
                }
            }

            return Path.Len() == Root.Len() || Path[Root.Len()] == '/';
        }
    }

    EAssetSource ClassifyAssetPath(FStringView AssetPath)
    {
        if (HasRoot(AssetPath, "/Game"))
        {
            return EAssetSource::Project;
        }

        if (HasRoot(AssetPath, "/Engine") || HasRoot(AssetPath, "/Editor"))
        {
            return EAssetSource::Engine;
        }

        return EAssetSource::Plugin;
    }

    FState& GetState()
    {
        static FState State;
        return State;
    }

    bool PassesSourceFilter(FStringView AssetPath)
    {
        const FState& State = GetState();
        if (!State.IsAnyActive())
        {
            return true;
        }

        switch (ClassifyAssetPath(AssetPath))
        {
        case EAssetSource::Project: return !State.bHideProjectContent;
        case EAssetSource::Engine:  return !State.bHideEngineContent;
        case EAssetSource::Plugin:  return !State.bHidePluginContent;
        }

        return true;
    }

    void DrawFilterButton(float ButtonWidth)
    {
        FState& State = GetState();
        const bool bActive = State.IsAnyActive();

        if (bActive)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, EditorColors::Accent());
            ImGui::PushStyleColor(ImGuiCol_Text, EditorColors::TextPrimary());
        }

        if (ImGui::Button(LE_ICON_FILTER, ImVec2(ButtonWidth, 0.0f)))
        {
            ImGui::OpenPopup("##AssetSourceFilter");
        }

        if (bActive)
        {
            ImGui::PopStyleColor(2);
        }

        ImGuiX::TextTooltip("{}", bActive
            ? "Filter by source. Some assets are hidden."
            : "Filter by source (engine, plugin, project content).");

        if (ImGui::BeginPopup("##AssetSourceFilter"))
        {
            ImGui::TextColored(EditorColors::TextMuted(), "SOURCES");
            ImGui::Separator();

            ImGui::Checkbox("Hide Project Content", &State.bHideProjectContent);
            ImGui::Checkbox("Hide Engine Content",  &State.bHideEngineContent);
            ImGui::Checkbox("Hide Plugin Content",  &State.bHidePluginContent);

            ImGui::Separator();

            ImGui::BeginDisabled(!State.IsAnyActive());
            if (ImGui::MenuItem(LE_ICON_CLOSE_CIRCLE " Clear Filters"))
            {
                State = FState();
            }
            ImGui::EndDisabled();

            ImGui::EndPopup();
        }
    }

    FAssetDataPtr GetSelectedAssetOfClass(const CClass* Class)
    {
        const IEditorToolContext* Tools = SessionOps::GetToolContext();
        FAssetDataPtr Selected = Tools != nullptr ? Tools->GetContentBrowserSelectedAsset() : nullptr;
        if (Selected == nullptr || Class == nullptr)
        {
            return nullptr;
        }

        // Null when the asset's class is not registered, e.g. it belongs to an unloaded plugin.
        const CClass* SelectedClass = FindObject<CClass>(Selected->AssetClass);
        return SelectedClass != nullptr && SelectedClass->IsChildOf(Class) ? Selected : nullptr;
    }

    bool DrawUseSelectedButton(bool bSelectionFits, const char* Hint)
    {
        ImGui::BeginDisabled(!bSelectionFits);
        const bool bPressed = ImGui::Button(LE_ICON_ARROW_LEFT_CIRCLE "##UseSelected", kButtonSize);
        ImGui::EndDisabled();
        ImGuiX::TextTooltip("{}", bSelectionFits ? "Use the asset selected in the Content Browser." : Hint);
        return bPressed;
    }

    void DrawBrowseButton(FStringView AssetPath)
    {
        ImGui::BeginDisabled(AssetPath.IsEmpty());
        if (ImGui::Button(LE_ICON_MAGNIFY "##Browse", kButtonSize))
        {
            if (IEditorToolContext* Tools = SessionOps::GetToolContext())
            {
                Tools->BrowseToAsset(AssetPath);
            }
        }
        ImGui::EndDisabled();
        ImGuiX::TextTooltip("{}", "Show this asset in the Content Browser.");
    }

    bool DrawClearButton(const char* Tooltip)
    {
        const bool bPressed = ImGuiX::ToneButton(LE_ICON_CLOSE_CIRCLE "##Clear", ImGuiX::EButtonTone::Danger, kButtonSize);
        ImGuiX::TextTooltip("{}", Tooltip);
        return bPressed;
    }

    const TVector<FAssetDataPtr>& FAssetsOfClassCache::Get(const CClass* Class)
    {
        if (!StaleLatch.Refresh(Class, CachedClass))
        {
            return Assets;
        }

        if (Class == nullptr)
        {
            Assets.Reset();
            return Assets;
        }

        Assets = FAssetRegistry::Get().FindByPredicate([Class](const FAssetData& Data)
        {
            // Null when the asset's class is not registered, e.g. it belongs to an unloaded plugin.
            const CClass* DataClass = FindObject<CClass>(Data.AssetClass);
            return DataClass != nullptr && DataClass->IsChildOf(Class);
        });

        Algo::Sort(Assets, [](const FAssetDataPtr& A, const FAssetDataPtr& B)
        {
            return strcmp(*A->AssetName, *B->AssetName) < 0;
        });

        return Assets;
    }
}
