#pragma once

#include "UI/Tools/NodeGraph/Animation/AnimGraphNode.h"
#include "AnimGraphNode_StateRouting.generated.h"

namespace Lumina
{
    // Stands in for a named set of states, so one wire authors the same edge out of all of them.
    REFLECT()
    class CAnimGraphNode_StateAlias : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        CClass* GetSupportedGraphClass() const override;

        FStringView GetNodeDisplayName() const override { return "State Alias"; }
        FStringView GetNodeTooltip() const override { return "A transition drawn out of this is compiled once per listed state, which is how you write 'from any of these' without the from-everywhere reach of Any State. List the states an edge should leave from."; }
        FFixedString GetNodeCategory() const override { return "State Machine"; }
        uint32 GetNodeTitleColor() const override { return IM_COL32(120, 100, 60, 255); }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override {}

        FString GetNodeTitleText() const override;

        /** States this alias stands for. An empty list compiles nothing and warns. */
        PROPERTY(Editable, Category = "Alias", Picker = "AnimState")
        TVector<FName> SourceStates;

        CAnimGraphPin* OutPin = nullptr;
    };

    // A routing hub with an edge in and edges out, compiled away into the direct edges it stands for.
    REFLECT()
    class CAnimGraphNode_StateConduit : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        CClass* GetSupportedGraphClass() const override;

        FStringView GetNodeDisplayName() const override { return "Conduit"; }
        FStringView GetNodeTooltip() const override { return "Shares one entry rule across several exits. Each in-edge is paired with each out-edge into a direct transition whose conditions are both rules ANDed, so the shared test is authored once. The out-edge supplies the blend duration."; }
        FFixedString GetNodeCategory() const override { return "State Machine"; }
        uint32 GetNodeTitleColor() const override { return IM_COL32(70, 110, 130, 255); }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override {}

        FString GetNodeTitleText() const override;

        /** Label shown on the node, purely for reading the graph. */
        PROPERTY(Editable, Category = "Conduit")
        FName ConduitName;

        CAnimGraphPin* InPin = nullptr;
        CAnimGraphPin* OutPin = nullptr;
    };
}
