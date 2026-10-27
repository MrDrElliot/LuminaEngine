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

        // Duplicates make the match ambiguous, so flag it rather than binding to whichever comes first.
        if (CEdNodeGraph* Graph = GetOwningGraph())
        {
            int32 MatchCount = 0;
            for (const auto& NodeRef : Graph->Nodes)
            {
                CEdGraphNode* Node = NodeRef.Get();
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

        for (const auto& NodeRef : Graph->Nodes)
        {
            CEdGraphNode* Node = NodeRef.Get();
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

        // Listed by the graph, since a popup window cannot open from inside the zoomed canvas.
        if (ImGui::Button(Preview, ImVec2(140.0f, 0.0f)) && Graph != nullptr)
        {
            TVector<FString> Declared;
            int32 Current = 0;
            for (const auto& NodeRef : Graph->Nodes)
            {
                const CMaterialNamedRerouteDeclaration* Declaration = Cast<CMaterialNamedRerouteDeclaration>(NodeRef.Get());
                if (Declaration != nullptr && !Declaration->Name.empty())
                {
                    Current = Declaration->Name == Name ? (int32)Declared.size() : Current;
                    Declared.push_back(Declaration->Name);
                }
            }

            Graph->OpenChoicePopup(this, Declared, Current, [this, Declared](int32 Chosen)
            {
                Name = Declared[Chosen];
                NotifyValueEdited();
            });
        }

        ImGui::PopID();

        // An unresolved usage compiles as if unconnected, which is silent, so flag it on the node.
        if (FindDeclaration() == nullptr)
        {
            ImGui::TextColored(ImVec4(0.96f, 0.36f, 0.38f, 1.0f), "Unresolved");
        }
    }
}
