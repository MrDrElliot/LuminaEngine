#pragma once
#include "Core/Object/ObjectMacros.h"
#include "UI/Tools/NodeGraph/EdGraphNode.h"
#include "MaterialNamedReroute.generated.h"

namespace Lumina
{
    class CClass;
    class CMaterialInput;
    class CMaterialOutput;
}

namespace Lumina
{
    // Declares a wireless link. Whatever feeds its input can be read anywhere else in the graph by a
    // usage node carrying the same name, with no wire drawn between them.
    //
    // Reports IsRerouteNode so the compiler emits nothing for it and every existing reroute walk
    // resolves straight through to its source, but opts out of dot rendering because it has a name
    // to show.
    REFLECT()
    class CMaterialNamedRerouteDeclaration : public CEdGraphNode
    {
        GENERATED_BODY()
    public:

        // Not a CMaterialGraphNode, so it names its graph itself.
        CClass* GetSupportedGraphClass() const override;

        void BuildNode() override;

        bool IsRerouteNode() const override { return true; }
        bool WantsRerouteDotRendering() const override { return false; }

        FStringView GetNodeDisplayName() const override { return "Named Reroute Declaration"; }
        FFixedString GetNodeCategory() const override { return "Utility"; }
        FStringView GetNodeTooltip() const override
        {
            return "Names whatever is plugged into it. A Named Reroute Usage anywhere in the graph "
                   "reads the same value without a wire.";
        }

        uint32 GetNodeTitleColor() const override { return IM_COL32(60, 105, 145, 255); }

        // The base returns the title's own text width, which the header layout then adds a second time
        // as trailing filler. That doubles the width of any node with a long name, and these have the
        // longest names in the graph. Zero lets the header size to the title alone.
        ImVec2 GetMinNodeTitleBarSize() const override { return ImVec2(0.0f, 28.0f); }

        void DrawNodeBody() override;

        FStringView GetRerouteName() const { return FStringView(Name.c_str(), Name.size()); }

        /** Name usage nodes match on. Must be unique within the graph. */
        PROPERTY(Editable, Category = "Named Reroute")
        FString Name = "NewNamedReroute";

        CMaterialInput* Input = nullptr;
    };

    // Reads a declaration's source by name. Has an output pin and no input; the link is resolved
    // through GetRerouteSourcePin, and GetImplicitInputNode is what keeps the declaration's upstream
    // reachable and ordered ahead of this node.
    REFLECT()
    class CMaterialNamedRerouteUsage : public CEdGraphNode
    {
        GENERATED_BODY()
    public:

        // Not a CMaterialGraphNode, so it names its graph itself.
        CClass* GetSupportedGraphClass() const override;

        void BuildNode() override;

        bool IsRerouteNode() const override { return true; }
        bool WantsRerouteDotRendering() const override { return false; }

        CEdNodeGraphPin* GetRerouteSourcePin() const override;
        CEdGraphNode* GetImplicitInputNode() const override;

        FStringView GetNodeDisplayName() const override { return "Named Reroute Usage"; }
        FFixedString GetNodeCategory() const override { return "Utility"; }
        FStringView GetNodeTooltip() const override
        {
            return "Reads a Named Reroute Declaration by name, with no wire between them.";
        }

        uint32 GetNodeTitleColor() const override { return IM_COL32(60, 105, 145, 255); }

        // The base returns the title's own text width, which the header layout then adds a second time
        // as trailing filler. That doubles the width of any node with a long name, and these have the
        // longest names in the graph. Zero lets the header size to the title alone.
        ImVec2 GetMinNodeTitleBarSize() const override { return ImVec2(0.0f, 28.0f); }

        void DrawNodeBody() override;

        /** Name of the declaration this reads from. */
        PROPERTY(Editable, Category = "Named Reroute")
        FString Name;

        CMaterialOutput* Output = nullptr;

    private:

        CMaterialNamedRerouteDeclaration* FindDeclaration() const;
    };
}
