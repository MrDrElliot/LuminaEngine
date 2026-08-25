#pragma once

#include "Audio/Graph/AudioGraphTypes.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "UI/Tools/NodeGraph/EdGraphNode.h"
#include "AudioGraphNode.generated.h"

namespace Lumina
{
    class CAudioGraphPin;

    /** Base for every audio graph node, mapping declared pins onto the operator the compiler emits. */
    REFLECT(NotPlaceable)
    class CAudioGraphNode : public CEdGraphNode
    {
        GENERATED_BODY()

    public:

        CClass* GetSupportedGraphClass() const override;

        uint32 GetNodeTitleColor() const override { return IM_COL32(38, 92, 130, 255); }
        ImVec2 GetMinNodeBodySize() const override { return ImVec2(120, 30); }

        /** Runtime operator this node compiles to. Empty for a node the compiler handles itself. */
        virtual FName GetOperatorName() const { return FName(); }

    protected:

        // Binds the pin to the node property whose name matches, spaces removed, when one exists.
        CAudioGraphPin* CreateInputPin(const char* PinName, EAudioGraphType Type);
        CAudioGraphPin* CreateOutputPin(const char* PinName, EAudioGraphType Type);
    };

    /** Declares a named value gameplay can write on a live instance. */
    REFLECT()
    class CAudioGraphInputNode : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        void DrawNodeBody() override;

        FStringView GetNodeDisplayName() const override { return "Graph Input"; }
        FStringView GetNodeTooltip() const override { return "Exposes a named parameter gameplay can drive."; }
        FFixedString GetNodeCategory() const override { return "Interface"; }
        FString GetNodeTitleText() const override;
        uint32 GetNodeTitleColor() const override { return IM_COL32(30, 110, 70, 255); }

        bool GetRenameText(FString& OutText) const override;
        void SetRenameText(const FString& InText) override;

        /** Name gameplay addresses this parameter by. */
        PROPERTY(Editable, Category = "Input")
        FName ParameterName = "Parameter";

        /** Value kind this parameter carries. Changing it rebuilds the pin and drops its wire. */
        PROPERTY(Editable, Category = "Input")
        EAudioGraphType Type = EAudioGraphType::Float;

        PROPERTY(Editable, Category = "Input")
        float DefaultFloat = 0.0f;

        PROPERTY(Editable, Category = "Input")
        int32 DefaultInt = 0;

        PROPERTY(Editable, Category = "Input")
        bool DefaultBool = false;

    private:

        EAudioGraphType BuiltType = EAudioGraphType::Invalid;
    };

    /** Publishes a float the game thread can read back off a playing instance. */
    REFLECT()
    class CAudioGraphNamedOutputNode : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;

        FStringView GetNodeDisplayName() const override { return "Graph Output"; }
        FStringView GetNodeTooltip() const override { return "Publishes a float the game thread can read each block."; }
        FFixedString GetNodeCategory() const override { return "Interface"; }
        FString GetNodeTitleText() const override;
        uint32 GetNodeTitleColor() const override { return IM_COL32(110, 80, 30, 255); }

        bool GetRenameText(FString& OutText) const override;
        void SetRenameText(const FString& InText) override;

        PROPERTY(Editable, Category = "Output")
        FName ParameterName = "Output";
    };

    /** Publishes a trigger the game thread can count, so a graph can drive gameplay events. */
    REFLECT()
    class CAudioGraphTriggerOutputNode : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;

        FStringView GetNodeDisplayName() const override { return "Graph Trigger Output"; }
        FStringView GetNodeTooltip() const override { return "Raises a named event gameplay can poll with ConsumeTriggerOutput."; }
        FFixedString GetNodeCategory() const override { return "Interface"; }
        FString GetNodeTitleText() const override;
        uint32 GetNodeTitleColor() const override { return IM_COL32(110, 80, 30, 255); }

        bool GetRenameText(FString& OutText) const override;
        void SetRenameText(const FString& InText) override;

        PROPERTY(Editable, Category = "Output")
        FName ParameterName = "OnEvent";
    };

    /** The graph's root. Everything the compiler emits is reachable backwards from this node. */
    REFLECT(NotPlaceable)
    class CAudioGraphOutputNode : public CAudioGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;

        FStringView GetNodeDisplayName() const override { return "Output"; }
        FStringView GetNodeTooltip() const override { return "Final stereo output and the end of playback signal."; }
        FFixedString GetNodeCategory() const override { return "Interface"; }
        uint32 GetNodeTitleColor() const override { return IM_COL32(140, 45, 45, 255); }

        bool IsDeletable() const override { return false; }
    };
}
