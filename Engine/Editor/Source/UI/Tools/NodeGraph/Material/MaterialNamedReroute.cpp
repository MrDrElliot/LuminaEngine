#include "MaterialNamedReroute.h"

#include "MaterialInput.h"
#include "MaterialOutput.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "MaterialNodeGraph.h"
#include "UI/Tools/NodeGraph/EdNodeGraph.h"
#include "UI/Tools/NodeGraph/EdNodeGraphPin.h"

namespace Lumina
{
    CClass* CMaterialNamedRerouteDeclaration::GetSupportedGraphClass() const
    {
        return CMaterialNodeGraph::StaticClass();
    }

    CClass* CMaterialNamedRerouteUsage::GetSupportedGraphClass() const
    {
        return CMaterialNodeGraph::StaticClass();
    }

    namespace
    {
        constexpr size_t GNameBufferSize = 64;

        // Shared by both nodes so the declaration and the usage always agree on what a name looks like.
        bool DrawNameField(const char* Id, FString& Name, float Width)
        {
            char Buffer[GNameBufferSize];
            const size_t Length = Name.size() < GNameBufferSize - 1 ? Name.size() : GNameBufferSize - 1;
            memcpy(Buffer, Name.c_str(), Length);
            Buffer[Length] = '\0';

            ImGui::SetNextItemWidth(Width);

            if (ImGui::InputText(Id, Buffer, sizeof(Buffer)))
            {
                Name = Buffer;
                return true;
            }

            return false;
        }
    }

    void CMaterialNamedRerouteDeclaration::BuildNode()
    {
        Input = static_cast<CMaterialInput*>(CreatePin(CMaterialInput::StaticClass(), "", ENodePinDirection::Input));
        Input->SetShouldDrawEditor(false);
    }

    void CMaterialNamedRerouteDeclaration::DrawNodeBody()
    {
        if (DrawNameField("##RerouteName", Name, 140.0f))
        {
            NotifyValueEdited();
        }

        // Duplicates make the match ambiguous, so say so on the node rather than silently binding
        // usages to whichever one happens to come first in the node list.
        if (CEdNodeGraph* Graph = GetOwningGraph())
        {
            int32 MatchCount = 0;
            for (CEdGraphNode* Node : Graph->Nodes)
            {
                CMaterialNamedRerouteDeclaration* Other = Cast<CMaterialNamedRerouteDeclaration>(Node);
                if (Other != nullptr && Other->Name == Name)
                {
                    ++MatchCount;
                }
            }

            if (MatchCount > 1)
            {
                ImGui::TextColored(ImVec4(0.96f, 0.55f, 0.30f, 1.0f), "Duplicate name");
            }
        }
    }

    void CMaterialNamedRerouteUsage::BuildNode()
    {
        Output = static_cast<CMaterialOutput*>(CreatePin(CMaterialOutput::StaticClass(), "", ENodePinDirection::Output));
        Output->SetShouldDrawEditor(false);
    }

    CMaterialNamedRerouteDeclaration* CMaterialNamedRerouteUsage::FindDeclaration() const
    {
        CEdNodeGraph* Graph = GetOwningGraph();
        if (Graph == nullptr || Name.empty())
        {
            return nullptr;
        }

        for (CEdGraphNode* Node : Graph->Nodes)
        {
            CMaterialNamedRerouteDeclaration* Declaration = Cast<CMaterialNamedRerouteDeclaration>(Node);
            if (Declaration != nullptr && Declaration->Name == Name)
            {
                return Declaration;
            }
        }

        return nullptr;
    }

    CEdNodeGraphPin* CMaterialNamedRerouteUsage::GetRerouteSourcePin() const
    {
        CMaterialNamedRerouteDeclaration* Declaration = FindDeclaration();
        return Declaration != nullptr ? Declaration->Input : nullptr;
    }

    CEdGraphNode* CMaterialNamedRerouteUsage::GetImplicitInputNode() const
    {
        return FindDeclaration();
    }

    void CMaterialNamedRerouteUsage::DrawNodeBody()
    {
        CEdNodeGraph* Graph = GetOwningGraph();

        const char* Preview = Name.empty() ? "(none)" : Name.c_str();

        ImGui::PushID(this);

        if (ImGui::Button(Preview, ImVec2(140.0f, 0.0f)))
        {
            ImGui::OpenPopup("##RerouteTargetPicker");
        }

        if (ImGui::BeginPopup("##RerouteTargetPicker"))
        {
            if (Graph != nullptr)
            {
                for (CEdGraphNode* Node : Graph->Nodes)
                {
                    CMaterialNamedRerouteDeclaration* Declaration = Cast<CMaterialNamedRerouteDeclaration>(Node);
                    if (Declaration == nullptr || Declaration->Name.empty())
                    {
                        continue;
                    }

                    const bool bSelected = Declaration->Name == Name;
                    if (ImGui::Selectable(Declaration->Name.c_str(), bSelected))
                    {
                        Name = Declaration->Name;
                        NotifyValueEdited();
                    }
                }
            }

            ImGui::EndPopup();
        }

        ImGui::PopID();

        // An unresolved usage compiles as if its input were simply unconnected, which is silent.
        // Flag it on the node so a renamed or deleted declaration is obvious.
        if (FindDeclaration() == nullptr)
        {
            ImGui::TextColored(ImVec4(0.96f, 0.36f, 0.38f, 1.0f), "Unresolved");
        }
    }
}
