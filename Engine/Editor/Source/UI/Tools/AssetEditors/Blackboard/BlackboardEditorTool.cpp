#include "BlackboardEditorTool.h"

#include "Assets/AssetTypes/Blackboard/Blackboard.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectArray.h"
#include "Core/Object/Package/Package.h"
#include "Tools/UI/ImGui/ImGuiDragDrop.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "imgui.h"

namespace Lumina
{
    static const char* BlackboardWindowName = "Blackboard";

    // Index-aligned with EBlackboardKeyType.
    static const char* KeyTypeLabels[] = { "Float", "Int", "Bool", "Enum", "Vector", "Object", "Entity" };

    // Per-type accent so the list reads at a glance.
    static ImU32 KeyTypeColor(EBlackboardKeyType Type)
    {
        switch (Type)
        {
        case EBlackboardKeyType::Float:  return IM_COL32(110, 170, 240, 255);
        case EBlackboardKeyType::Int:    return IM_COL32( 90, 200, 160, 255);
        case EBlackboardKeyType::Bool:   return IM_COL32(225, 150, 110, 255);
        case EBlackboardKeyType::Enum:   return IM_COL32(200, 160, 240, 255);
        case EBlackboardKeyType::Vector: return IM_COL32(240, 210, 110, 255);
        case EBlackboardKeyType::Object: return IM_COL32(235, 130, 150, 255);
        case EBlackboardKeyType::Entity: return IM_COL32(120, 220, 220, 255);
        }
        return IM_COL32(200, 200, 200, 255);
    }

    FBlackboardEditorTool::FBlackboardEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset)
    {
    }

    void FBlackboardEditorTool::OnInitialize()
    {
        FAssetEditorTool::OnInitialize();

        CreateToolWindow(BlackboardWindowName, [this](bool /*bFocused*/)
        {
            DrawKeysWindow();
        });
    }

    void FBlackboardEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& /*InDockspaceSize*/) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(BlackboardWindowName).c_str(), InDockspaceID);
    }

    void FBlackboardEditorTool::EnsureEnumCacheBuilt()
    {
        if (bEnumCacheBuilt)
        {
            return;
        }
        bEnumCacheBuilt = true;

        GObjectArray.ForEachObject([&](CObjectBase* Object, int32)
        {
            if (Object != nullptr && Object->IsA<CEnum>())
            {
                CEnum* Enum = static_cast<CEnum*>(Object);
                if (!Enum->Names.empty())
                {
                    ReflectedEnums.push_back(Enum);
                    EnumsByName[Enum->GetName()] = Enum;
                }
            }
        });

        eastl::sort(ReflectedEnums.begin(), ReflectedEnums.end(), [](CEnum* A, CEnum* B)
        {
            return strcmp(A->MakeDisplayName().c_str(), B->MakeDisplayName().c_str()) < 0;
        });
    }

    CEnum* FBlackboardEditorTool::FindEnum(const FName& Name) const
    {
        auto It = EnumsByName.find(Name);
        return It == EnumsByName.end() ? nullptr : It->second;
    }

    void FBlackboardEditorTool::DrawEnumTypeCombo(FBlackboardKey& Key)
    {
        EnsureEnumCacheBuilt();

        CEnum* Current = FindEnum(Key.EnumType);
        const FFixedString Preview = Current ? Current->MakeDisplayName() : FFixedString("(select enum)");

        int32 CurrentIndex = INDEX_NONE;
        for (int32 i = 0; i < (int32)ReflectedEnums.size(); ++i)
        {
            if (ReflectedEnums[i] == Current)
            {
                CurrentIndex = i;
                break;
            }
        }

        ImGui::SetNextItemWidth(-1.0f);
        const int32 Picked = ImGuiX::SearchableCombo("##enumtype", Preview.c_str(), (int32)ReflectedEnums.size(), CurrentIndex,
            [this](int32 Index) { return ReflectedEnums[Index]->MakeDisplayName(); }, LE_ICON_CODE_BRACES);

        if (Picked != INDEX_NONE)
        {
            CEnum* Enum = ReflectedEnums[Picked];
            Key.EnumType = Enum->GetName();
            Key.DefaultInt = Enum->Names.empty() ? 0 : (int32)Enum->GetValueAtIndex(0);
            GetAsset<CBlackboard>()->GetPackage()->MarkDirty();
        }
    }

    void FBlackboardEditorTool::DrawDefaultEditor(FBlackboardKey& Key)
    {
        CBlackboard* Blackboard = GetAsset<CBlackboard>();

        switch (Key.Type)
        {
        case EBlackboardKeyType::Float:
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::DragFloat("##defF", &Key.DefaultFloat, 0.01f);
            if (ImGui::IsItemDeactivatedAfterEdit()) Blackboard->GetPackage()->MarkDirty();
            break;

        case EBlackboardKeyType::Int:
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::DragInt("##defI", &Key.DefaultInt);
            if (ImGui::IsItemDeactivatedAfterEdit()) Blackboard->GetPackage()->MarkDirty();
            break;

        case EBlackboardKeyType::Bool:
            if (ImGui::Checkbox("##defB", &Key.DefaultBool)) Blackboard->GetPackage()->MarkDirty();
            break;

        case EBlackboardKeyType::Enum:
        {
            DrawEnumTypeCombo(Key);

            CEnum* Enum = FindEnum(Key.EnumType);
            if (Enum != nullptr)
            {
                int32 CurrentIndex = INDEX_NONE;
                for (int64 i = 0; i < (int64)Enum->Names.size(); ++i)
                {
                    if ((int32)Enum->GetValueAtIndex(i) == Key.DefaultInt)
                    {
                        CurrentIndex = (int32)i;
                        break;
                    }
                }

                const FFixedString ValuePreview = Enum->GetNameAtValue((uint64)Key.DefaultInt).c_str();
                ImGui::SetNextItemWidth(-1.0f);
                const int32 Picked = ImGuiX::SearchableCombo("##enumval", ValuePreview.c_str(), (int32)Enum->Names.size(), CurrentIndex,
                    [Enum](int32 Index) { return FFixedString(Enum->GetNameAtIndex(Index).c_str()); }, LE_ICON_RHOMBUS_OUTLINE);

                if (Picked != INDEX_NONE)
                {
                    Key.DefaultInt = (int32)Enum->GetValueAtIndex(Picked);
                    Blackboard->GetPackage()->MarkDirty();
                }
            }
            else
            {
                ImGui::TextDisabled("pick an enum type");
            }
            break;
        }

        case EBlackboardKeyType::Vector:
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::DragFloat3("##defV", &Key.DefaultVector.x, 0.01f);
            if (ImGui::IsItemDeactivatedAfterEdit()) Blackboard->GetPackage()->MarkDirty();
            break;

        case EBlackboardKeyType::Object:
        {
            const char* Label = Key.DefaultObject.IsValid() ? Key.DefaultObject->GetName().c_str() : "<None> (drop asset)";
            ImGui::Button(Label, ImVec2(-1.0f, 0.0f));
            if (ImGui::BeginDragDropTarget())
            {
                if (CObject* Dropped = DragDrop::AcceptAssetOfClass(CObject::StaticClass()))
                {
                    Key.DefaultObject = Dropped;
                    Blackboard->GetPackage()->MarkDirty();
                }
                ImGui::EndDragDropTarget();
            }
            break;
        }

        case EBlackboardKeyType::Entity:
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Set at runtime");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGuiX::TextTooltip_Internal("An entity handle only means something inside a live world, "
                    "so Entity keys start unset and are written by scripts or systems.");
            break;
        }
    }

    void FBlackboardEditorTool::DrawKeysWindow()
    {
        CBlackboard* Blackboard = GetAsset<CBlackboard>();
        if (Blackboard == nullptr)
        {
            return;
        }

        ImGui::TextWrapped("Keys declared here are the schema for this blackboard. Entities get their own "
            "values via a Blackboard Component seeded from these defaults; the animation graph and AI read "
            "and write by key name.");
        ImGui::Separator();

        if (ImGui::Button(LE_ICON_PLUS " Add Key"))
        {
            FBlackboardKey Key;
            Key.Name = FName(FString("NewKey_") + eastl::to_string(Blackboard->Keys.size()));
            Blackboard->Keys.push_back(Key);
            Blackboard->GetPackage()->MarkDirty();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%d key%s", (int)Blackboard->Keys.size(), Blackboard->Keys.size() == 1 ? "" : "s");

        ImGui::Spacing();

        if (Blackboard->Keys.empty())
        {
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.35f, 1.0f), "No keys yet -- add one above.");
            return;
        }

        int32 PendingRemoval = INDEX_NONE;

        // Cap card width so entries don't stretch across a wide window.
        constexpr float MaxCardWidth = 400.0f;
        const float CardWidth = ImGui::GetContentRegionAvail().x < MaxCardWidth
            ? ImGui::GetContentRegionAvail().x
            : MaxCardWidth;

        for (int32 i = 0; i < (int32)Blackboard->Keys.size(); ++i)
        {
            FBlackboardKey& Key = Blackboard->Keys[i];
            ImGui::PushID(i);

            // Each key is its own bordered, auto-height card with stacked fields.
            if (ImGui::BeginChild("##card", ImVec2(CardWidth, 0.0f), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders))
            {
                // -- Header: type accent dot + name + delete -------------------
                const ImVec2 DotPos = ImGui::GetCursorScreenPos();
                const float  RowH   = ImGui::GetFrameHeight();
                ImGui::Dummy(ImVec2(14.0f, RowH));
                ImGui::GetWindowDrawList()->AddCircleFilled(
                    ImVec2(DotPos.x + 6.0f, DotPos.y + RowH * 0.5f), 5.0f, KeyTypeColor(Key.Type));
                ImGui::SameLine();

                bool bDuplicate = false;
                for (int32 j = 0; j < (int32)Blackboard->Keys.size(); ++j)
                {
                    if (j != i && !Key.Name.IsNone() && Blackboard->Keys[j].Name == Key.Name)
                    {
                        bDuplicate = true;
                        break;
                    }
                }

                char NameBuffer[128];
                const FString NameStr = Key.Name.ToString();
                strncpy(NameBuffer, NameStr.c_str(), sizeof(NameBuffer));
                NameBuffer[sizeof(NameBuffer) - 1] = '\0';

                const float DeleteW = 28.0f;
                if (bDuplicate) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - DeleteW);
                if (ImGui::InputTextWithHint("##name", "Key name", NameBuffer, sizeof(NameBuffer)))
                {
                    Key.Name = FName(NameBuffer);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) Blackboard->GetPackage()->MarkDirty();
                if (bDuplicate)
                {
                    ImGui::PopStyleColor();
                    ImGuiX::TextTooltip_Internal("Duplicate key name");
                }

                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.20f, 0.20f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.25f, 0.25f, 1.0f));
                if (ImGui::Button(LE_ICON_TRASH_CAN, ImVec2(DeleteW - 4.0f, 0.0f)))
                {
                    PendingRemoval = i;
                }
                ImGui::PopStyleColor(2);

                // -- Fields: aligned label / value rows ------------------------
                if (ImGui::BeginTable("##fields", 2, ImGuiTableFlags_SizingFixedFit))
                {
                    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

                    // Type.
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted("Type");
                    ImGui::TableSetColumnIndex(1);
                    int CurrentType = (int)Key.Type;
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::Combo("##type", &CurrentType, KeyTypeLabels, IM_ARRAYSIZE(KeyTypeLabels)))
                    {
                        Key.Type = (EBlackboardKeyType)CurrentType;
                        Blackboard->GetPackage()->MarkDirty();
                    }

                    // Default.
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted("Default");
                    ImGui::TableSetColumnIndex(1);
                    DrawDefaultEditor(Key);

                    // Flags.
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted("Flags");
                    ImGui::TableSetColumnIndex(1);
                    {
                        bool bReadOnly = EnumHasAnyFlags(Key.Flags, EBlackboardKeyFlags::ReadOnly);
                        if (ImGui::Checkbox("Read Only", &bReadOnly))
                        {
                            if (bReadOnly) EnumAddFlags(Key.Flags, EBlackboardKeyFlags::ReadOnly);
                            else           EnumRemoveFlags(Key.Flags, EBlackboardKeyFlags::ReadOnly);
                            Blackboard->GetPackage()->MarkDirty();
                        }
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                            ImGuiX::TextTooltip_Internal("Set from default; not written at runtime");

                        ImGui::SameLine(0.0f, 16.0f);
                        bool bHidden = EnumHasAnyFlags(Key.Flags, EBlackboardKeyFlags::Hidden);
                        if (ImGui::Checkbox("Hidden", &bHidden))
                        {
                            if (bHidden) EnumAddFlags(Key.Flags, EBlackboardKeyFlags::Hidden);
                            else          EnumRemoveFlags(Key.Flags, EBlackboardKeyFlags::Hidden);
                            Blackboard->GetPackage()->MarkDirty();
                        }
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                            ImGuiX::TextTooltip_Internal("Hidden from Get Parameter / transition pickers");
                    }

                    ImGui::EndTable();
                }
            }
            ImGui::EndChild();

            ImGui::PopID();
            ImGui::Spacing();
        }

        if (PendingRemoval != INDEX_NONE)
        {
            Blackboard->Keys.erase(Blackboard->Keys.begin() + PendingRemoval);
            Blackboard->GetPackage()->MarkDirty();
        }
    }
}
