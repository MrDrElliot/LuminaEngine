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

            // AllowOverlap so the remove button gets its own clicks instead of the header toggling collapse.
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
                        // The driver adopts an owner-less script next tick, the same path a deserialized script takes.
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
