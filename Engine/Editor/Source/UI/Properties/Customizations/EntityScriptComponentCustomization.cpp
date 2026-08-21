#include "EntityScriptComponentCustomization.h"

#include "imgui.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectIterator.h"
#include "Scripting/EntityScript.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "UI/Properties/PropertyTable.h"

namespace Lumina
{
    namespace
    {
        // Every concrete CEntityScript subclass in the project: native C++ scripts and minted C# ones alike,
        // because both are just CClasses deriving the same base. Excludes the base itself and class default
        // objects. Rebuilt per frame -- this is an inspector, and the set changes on every script hot reload.
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

    EPropertyChangeOp FEntityScriptComponentCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property)
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

            const FString HeaderLabel = (Script != nullptr && Script->GetClass() != nullptr)
                ? Script->GetClass()->GetName().ToString()
                : FString("(missing script)");

            const float ButtonWidth = ImGui::GetFrameHeight();
            const float HeaderRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;

            // AllowOverlap so the right-aligned remove button gets its own clicks instead of the header
            // swallowing them and toggling collapse.
            ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
            const bool bOpen = ImGui::CollapsingHeader((HeaderLabel + "##scripthdr").c_str(), ImGuiTreeNodeFlags_AllowOverlap);

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
                // A script's [Property] members are real FPropertys on its class, so the stock table draws
                // them -- no bespoke drawer, and native and C# scripts render identically.
                //
                // The class is compared as well as the pointer: a destroyed script can be followed by a
                // different one allocated at the same address, which the pointer test alone would miss and
                // then draw with the wrong layout.
                if (View.BoundScript != Script || View.BoundClass != Script->GetClass())
                {
                    View.ValueTable = MakeUnique<FPropertyTable>(static_cast<void*>(Script), static_cast<CStruct*>(Script->GetClass()));
                    View.ValueTable->SetShowSearchBar(false);

                    // The nested table is a separate FPropertyTable, so its edits do not pass through the
                    // outer one. Route them back out as this customization's own change op, which is what
                    // puts a script property edit under the same transaction and scene-dirty handling as
                    // every other property in the panel.
                    View.ValueTable->SetStartEditCallback ([this](const FPropertyChangedEvent&) { NestedChangeOp = EPropertyChangeOp::Started; });
                    View.ValueTable->SetPostEditCallback  ([this](const FPropertyChangedEvent&)
                    {
                        if (NestedChangeOp == EPropertyChangeOp::None) { NestedChangeOp = EPropertyChangeOp::Updated; }
                    });
                    View.ValueTable->SetFinishEditCallback([this](const FPropertyChangedEvent&) { NestedChangeOp = EPropertyChangeOp::Finished; });

                    View.BoundScript = Script;
                    View.BoundClass = Script->GetClass();
                }
                if (View.ValueTable)
                {
                    View.ValueTable->DrawTree();
                }
            }

            ImGui::PopID();
        }

        // Add picker. Listing classes rather than C# type names is what makes a C++ script selectable here.
        TVector<CClass*> Classes;
        GatherScriptClasses(Classes);

        if (ImGui::BeginCombo("##AddEntityScript", "Add Script..."))
        {
            for (CClass* Class : Classes)
            {
                const FString Name = Class->GetName().ToString();
                if (ImGui::Selectable(Name.c_str()))
                {
                    PendingMutation = [Component, Class]
                    {
                        // Constructed here rather than through EntityScripts::Attach: the customization has
                        // the component but not the registry/entity. The driver adopts an owner-less script
                        // on the next tick (running OnAttach then OnReady), which is the same path a
                        // deserialized script takes.
                        if (CObject* Created = NewObject(Class, nullptr, NAME_None, FGuid::New(), OF_Transient))
                        {
                            Component->Scripts.push_back(static_cast<CEntityScript*>(Created));
                        }
                    };
                    bWasChanged = true;
                }
            }
            ImGui::EndCombo();
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

        // An edit inside one of the nested script tables. Reported after the add/remove ops above so a
        // structural change to the script list always wins the frame.
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
        // Deliberately empty, and it must stay cheap and idempotent: UpdateAndDraw calls this on EVERY draw,
        // not only when something replaced the value. Clearing the cached tables here rebuilt them each frame,
        // which threw away every widget's in-progress state -- a field could be clicked but never edited.
        //
        // Rebinding is handled where it can be decided per slot instead: DrawProperty rebuilds a slot's table
        // only when that slot's script object or its class actually changed. A script-generation change is
        // handled further out still, by the details panel dropping every table (FSceneEditorTool).
    }
}
