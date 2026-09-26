#include "EntityScriptComponentCustomization.h"

#include "imgui.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectIterator.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Scripting/EntityScript.h"
#include "Scripting/ScriptableObject.h"
#include "Platform/Process/PlatformProcess.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Properties/PropertyTable.h"

namespace Lumina
{
    namespace
    {
        constexpr const char* GScriptIcon = LE_ICON_LANGUAGE_CSHARP;

        /** "Game.ZombieFPS.ZombieBrain" -> "ZombieBrain" plus "Game.ZombieFPS", so the name can lead. */
        void SplitTypeName(const FString& Full, FStringView& OutShort, FStringView& OutNamespace)
        {
            const FStringView View(Full);
            const size_t Dot = View.find_last_of('.');
            if (Dot == FStringView::npos)
            {
                OutShort = View;
                OutNamespace = FStringView();
                return;
            }
            OutShort = View.substr(Dot + 1);
            OutNamespace = View.substr(0, Dot);
        }

        // Rebuilt per frame, since this is an inspector and the set changes on every script hot reload.
        void GatherScriptClasses(TVector<CClass*>& Out)
        {
            Out.clear();
            CClass* Base = CEntityScript::StaticClass();

            for (TObjectIterator<CClass> It; It; ++It)
            {
                CClass* Class = *It;
                if (Class == nullptr || Class == Base || !Class->IsChildOf(Base))
                {
                    continue;
                }
                if (Class->HasAnyFlag(OF_DefaultObject) || Class->HasAnyFlag(OF_MarkedDestroy))
                {
                    continue;
                }
                Out.push_back(Class);
            }
        }
    }

    TSharedPtr<FEntityScriptComponentCustomization> FEntityScriptComponentCustomization::MakeInstance()
    {
        return MakeShared<FEntityScriptComponentCustomization>();
    }

    EPropertyChangeOp FEntityScriptComponentCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        bool bWasChanged = false;
        auto* Component = static_cast<SEntityScriptComponent*>(Property->ContainerPtr);

        ImGui::PushID(this);

        SlotViews.resize(Component->Scripts.size());

        for (int32 Index = 0; Index < (int32)Component->Scripts.size(); ++Index)
        {
            CEntityScript* Script = Component->Scripts[Index].Get();
            FSlotView& View = SlotViews[Index];
            ImGui::PushID(Index);

            const bool bValid = Script != nullptr && Script->GetClass() != nullptr;
            const FString TypeName = bValid ? Script->GetClass()->GetName().ToString() : FString();

            FStringView Short;
            FStringView Namespace;
            SplitTypeName(TypeName, Short, Namespace);

            const FString HeaderLabel = bValid
                ? Lumina::Format("{} {}", GScriptIcon, FString(Short.data(), Short.size()))
                : FString(LE_ICON_ALERT " (missing script)");

            const float ButtonWidth = ImGui::GetFrameHeight();
            const float Spacing     = ImGui::GetStyle().ItemSpacing.x;
            const float HeaderRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;

            // Breathing room around the title, which is what stops the stack of scripts reading as one block.
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 6.0f));
            ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
            // AllowOverlap so the buttons get their own clicks instead of the header toggling collapse.
            const bool bOpen = ImGui::CollapsingHeader((HeaderLabel + "##scripthdr").c_str(), ImGuiTreeNodeFlags_AllowOverlap);
            ImGui::PopStyleVar();

            // The namespace trails the name in the header's own row, dimmed, so it informs without competing.
            if (!Namespace.empty())
            {
                ImGui::SameLine();
                ImGui::TextDisabled("%.*s", (int)Namespace.size(), Namespace.data());
            }

            if (!View.SourceFile.empty())
            {
                ImGui::SameLine(HeaderRight - ButtonWidth * 2.0f - Spacing);
                if (ImGui::SmallButton(LE_ICON_FILE_CODE "##OpenEntityScriptSource"))
                {
                    Platform::OpenSourceFile(UTF8_TO_TCHAR(View.SourceFile.c_str()), 1);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Open %s", View.SourceFile.c_str());
                }
            }

            ImGui::SameLine(HeaderRight - ButtonWidth);
            if (ImGui::SmallButton(LE_ICON_DELETE "##RemoveEntityScript"))
            {
                PendingMutation = [Component, Index]
                {
                    if (Index < (int32)Component->Scripts.size())
                    {
                        if (CEntityScript* Removed = Component->Scripts[Index].Get(); Removed != nullptr && Removed->IsAttached())
                        {
                            Removed->OnDetach();
                        }
                        Component->Scripts.erase(Component->Scripts.begin() + Index);
                    }
                };
                bWasChanged = true;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Remove this script");
            }

            if (bOpen && Script != nullptr && Script->GetClass() != nullptr)
            {
                // The class is compared too, since a new script can land at a destroyed one's address.
                if (View.BoundScript != Script || View.BoundClass != Script->GetClass())
                {
                    View.ValueTable = MakeUnique<FPropertyTable>(static_cast<void*>(Script), static_cast<CStruct*>(Script->GetClass()));
                    View.ValueTable->SetShowSearchBar(false);

                    // Routed out as this customization's op, so a script edit shares the panel's transaction handling.
                    View.ValueTable->SetStartEditCallback ([this](const FPropertyChangedEvent&) { NestedChangeOp = EPropertyChangeOp::Started; });
                    View.ValueTable->SetPostEditCallback  ([this](const FPropertyChangedEvent&)
                    {
                        if (NestedChangeOp == EPropertyChangeOp::None) { NestedChangeOp = EPropertyChangeOp::Updated; }
                    });
                    View.ValueTable->SetFinishEditCallback([this](const FPropertyChangedEvent&) { NestedChangeOp = EPropertyChangeOp::Finished; });

                    DotNet::GatherScriptButtons(Script->GetClass()->GetName().ToString(), View.Buttons);
                    View.SourceFile = DotNet::FindScriptSourceFile(Script->GetClass()->GetName().ToString());

                    View.BoundScript = Script;
                    View.BoundClass = Script->GetClass();
                }
                if (View.ValueTable)
                {
                    View.ValueTable->DrawTree();
                }

                // The instance is created on demand, so a script that has never been dispatched to still has
                // something to invoke against.
                if (!View.Buttons.empty())
                {
                    ImGui::Spacing();

                    const float Right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
                    for (int32 ButtonIndex = 0; ButtonIndex < (int32)View.Buttons.size(); ++ButtonIndex)
                    {
                        const Scripting::FScriptButton& Button = View.Buttons[ButtonIndex];
                        const FString& Label = Button.Label.empty() ? Button.Method : Button.Label;
                        const FString Text = Lumina::Format("{} {}", LE_ICON_PLAY_CIRCLE, Label);
                        const float Width = ImGui::CalcTextSize(Text.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;

                        // Wrapped by hand, since a row of actions should use the width it has and then break.
                        if (ButtonIndex > 0 && ImGui::GetCursorPosX() + Width < Right)
                        {
                            ImGui::SameLine();
                        }

                        if (ImGui::Button(Text.c_str()))
                        {
                            if (void* Instance = Scriptable::GetOrCreateInstance(Script))
                            {
                                DotNet::InvokeScriptButton(Instance, Button.Method);
                            }
                        }
                        if (!Button.Tooltip.empty() && ImGui::IsItemHovered())
                        {
                            ImGui::SetTooltip("%s", Button.Tooltip.c_str());
                        }
                    }
                }

                ImGui::Spacing();
            }

            ImGui::PopID();
        }

        // Add picker. Listing classes rather than C# type names is what makes a C++ script selectable here.
        TVector<CClass*> Classes;
        GatherScriptClasses(Classes);

        ImGui::Separator();
        ImGui::Spacing();

        // Sized to its label rather than the panel: this is an action, and a full-width field reads as one
        // more empty property with the arrow stranded at the far edge.
        const char* AddLabel = LE_ICON_PLUS " Add Script";
        const float AddWidth = ImGui::CalcTextSize(AddLabel).x
            + ImGui::GetFrameHeight()
            + ImGui::GetStyle().FramePadding.x * 3.0f;
        ImGui::SetNextItemWidth(AddWidth);

        // Searchable, because a project accumulates far more script classes than a flat list stays usable for.
        const int32 Picked = ImGuiX::SearchableCombo("##AddEntityScript", AddLabel,
            (int32)Classes.size(), INDEX_NONE,
            [&Classes](int32 ItemIndex) -> FFixedString
            {
                FStringView Short;
                FStringView Namespace;
                const FString Name = Classes[ItemIndex]->GetName().ToString();
                SplitTypeName(Name, Short, Namespace);
                return Namespace.empty()
                    ? FFixedString(Short.data(), Short.size())
                    : FFixedString(Lumina::Format("{}   {}", FString(Short.data(), Short.size()),
                        FString(Namespace.data(), Namespace.size())).c_str());
            },
            GScriptIcon);

        if (Picked != INDEX_NONE && Picked < (int32)Classes.size())
        {
            CClass* Class = Classes[Picked];
            PendingMutation = [Component, Class]
            {
                // The driver adopts an owner-less script next tick, the same path a deserialized script takes.
                if (CObject* Created = NewObject(Class, nullptr, NAME_None, FGuid::New(), OF_Transient))
                {
                    Component->Scripts.push_back(static_cast<CEntityScript*>(Created));
                }
            };
            bWasChanged = true;
        }

        ImGui::PopID();

        if (bWasChanged)
        {
            if (bFinishPending)
            {
                return EPropertyChangeOp::Updated;
            }
            bFinishPending = true;
            return EPropertyChangeOp::Started;
        }
        if (bFinishPending)
        {
            bFinishPending = false;
            return EPropertyChangeOp::Finished;
        }

        // Reported after the add and remove ops, so a structural change always wins the frame.
        if (NestedChangeOp != EPropertyChangeOp::None)
        {
            const EPropertyChangeOp Op = NestedChangeOp;
            NestedChangeOp = EPropertyChangeOp::None;
            return Op;
        }
        return EPropertyChangeOp::None;
    }

    void FEntityScriptComponentCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        // Runs after BeginTransaction, so the undo snapshot captured the pre-change state.
        if (PendingMutation)
        {
            PendingMutation();
            PendingMutation = {};
        }
    }

    void FEntityScriptComponentCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        // Must stay cheap and idempotent, since UpdateAndDraw calls this on EVERY draw.
    }
}
