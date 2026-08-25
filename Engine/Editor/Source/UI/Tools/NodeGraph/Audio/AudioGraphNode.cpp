#include "EditorPCH.h"
#include "AudioGraphNode.h"

#include "AudioGraphPin.h"
#include "AudioNodeGraph.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "UI/Tools/NodeGraph/EdNodeGraph.h"
#include <imgui.h>

namespace Lumina
{
    namespace
    {
        /** Property name for a pin, which is its label with the spaces taken out. */
        FName PropertyNameForPin(const char* PinName)
        {
            FString Compact;
            for (const char* Cursor = PinName; *Cursor != '\0'; ++Cursor)
            {
                if (*Cursor != ' ')
                {
                    Compact += *Cursor;
                }
            }
            return FName(Compact);
        }
    }

    CClass* CAudioGraphNode::GetSupportedGraphClass() const
    {
        return CAudioNodeGraph::StaticClass();
    }

    CAudioGraphPin* CAudioGraphNode::CreateInputPin(const char* PinName, EAudioGraphType Type)
    {
        CAudioGraphPin* Pin = static_cast<CAudioGraphPin*>(
            CreatePin(CAudioGraphPin::StaticClass(), FString(PinName), ENodePinDirection::Input));

        Pin->SetPinType(Type);
        Pin->SetShouldDrawEditor(true);
        Pin->BindDefaultProperty(GetClass()->GetProperty(PropertyNameForPin(PinName)));

        return Pin;
    }

    CAudioGraphPin* CAudioGraphNode::CreateOutputPin(const char* PinName, EAudioGraphType Type)
    {
        CAudioGraphPin* Pin = static_cast<CAudioGraphPin*>(
            CreatePin(CAudioGraphPin::StaticClass(), FString(PinName), ENodePinDirection::Output));

        Pin->SetPinType(Type);
        return Pin;
    }

    void CAudioGraphInputNode::BuildNode()
    {
        CreateOutputPin("Value", Type);
        BuiltType = Type;
    }

    void CAudioGraphInputNode::DrawNodeBody()
    {
        if (BuiltType == Type)
        {
            return;
        }

        for (const TObjectPtr<CEdNodeGraphPin>& Pin : NodePins[(uint32)ENodePinDirection::Output])
        {
            if (Pin.IsValid())
            {
                Pin->ClearConnections();
            }
        }

        NodePins[(uint32)ENodePinDirection::Output].clear();
        BuildNode();

        if (CEdNodeGraph* Graph = GetOwningGraph())
        {
            Graph->ValidateGraph();
            Graph->NotifyContentChanged();
        }
    }

    FString CAudioGraphInputNode::GetNodeTitleText() const
    {
        return FString(ParameterName.c_str()) + " (" + ToString(Type) + ")";
    }

    bool CAudioGraphInputNode::GetRenameText(FString& OutText) const
    {
        OutText = ParameterName.c_str();
        return true;
    }

    void CAudioGraphInputNode::SetRenameText(const FString& InText)
    {
        ParameterName = FName(InText);
        NotifyValueEdited();
    }

    void CAudioGraphNamedOutputNode::BuildNode()
    {
        CreateInputPin("Value", EAudioGraphType::Float);
    }

    FString CAudioGraphNamedOutputNode::GetNodeTitleText() const
    {
        return FString(ParameterName.c_str()) + " (Output)";
    }

    bool CAudioGraphNamedOutputNode::GetRenameText(FString& OutText) const
    {
        OutText = ParameterName.c_str();
        return true;
    }

    void CAudioGraphNamedOutputNode::SetRenameText(const FString& InText)
    {
        ParameterName = FName(InText);
        NotifyValueEdited();
    }

    void CAudioGraphTriggerOutputNode::BuildNode()
    {
        CreateInputPin("Trigger", EAudioGraphType::Trigger);
    }

    FString CAudioGraphTriggerOutputNode::GetNodeTitleText() const
    {
        return FString(ParameterName.c_str()) + " (Trigger Out)";
    }

    bool CAudioGraphTriggerOutputNode::GetRenameText(FString& OutText) const
    {
        OutText = ParameterName.c_str();
        return true;
    }

    void CAudioGraphTriggerOutputNode::SetRenameText(const FString& InText)
    {
        ParameterName = FName(InText);
        NotifyValueEdited();
    }

    void CAudioGraphOutputNode::BuildNode()
    {
        CreateInputPin("Out Left", EAudioGraphType::Audio);
        CreateInputPin("Out Right", EAudioGraphType::Audio);
        CreateInputPin("On Finished", EAudioGraphType::Trigger);
    }
}
