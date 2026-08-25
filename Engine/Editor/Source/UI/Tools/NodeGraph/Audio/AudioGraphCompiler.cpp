#include "EditorPCH.h"
#include "AudioGraphCompiler.h"

#include "AudioGraphNode.h"
#include "AudioGraphPin.h"
#include "AudioNodeGraph.h"
#include "Assets/AssetTypes/Audio/AudioStream.h"
#include "Audio/Graph/AudioGraphInstance.h"
#include "Audio/Graph/AudioGraphOperator.h"
#include "Containers/HashTable.h"
#include "Core/Object/Cast.h"

namespace Lumina
{
    namespace
    {
        /** Producer pin feeding InputPin, looking through any passthrough node between them. */
        CEdNodeGraphPin* ResolveSourcePin(CEdNodeGraphPin* InputPin)
        {
            if (InputPin == nullptr || !InputPin->HasConnection())
            {
                return nullptr;
            }

            CEdNodeGraphPin* Source = InputPin->GetConnection(0);

            while (Source != nullptr && Source->GetOwningNode() != nullptr && Source->GetOwningNode()->IsRerouteNode())
            {
                CEdNodeGraphPin* Upstream = Source->GetOwningNode()->GetRerouteSourcePin();
                if (Upstream == nullptr || !Upstream->HasConnection())
                {
                    return nullptr;
                }
                Source = Upstream->GetConnection(0);
            }

            return Source;
        }

        struct FCompileContext
        {
            FAudioGraphCompileResult*          Result = nullptr;
            THashMap<CEdNodeGraphPin*, uint16> PinSlots;

            uint16 Allocate(EAudioGraphType Type)
            {
                return Result->Program.SlotCounts.Allocate(Type);
            }

            void Error(const FString& Message)
            {
                Result->Errors.push_back(Message);
            }

            // Deduplicated, because each entry costs a full PCM decode held for the life of the voice.
            int32 FindOrAddWave(CAudioStream* Wave)
            {
                for (int32 Index = 0; Index < (int32)Result->Waves.size(); ++Index)
                {
                    if (Result->Waves[Index] == Wave)
                    {
                        return Index;
                    }
                }

                Result->Waves.push_back(Wave);
                return (int32)Result->Waves.size() - 1;
            }
        };

        /** Depth first post order over the input closure, so a producer always lands before its consumer. */
        bool VisitNode(CEdGraphNode* Node, THashSet<CEdGraphNode*>& Visited, THashSet<CEdGraphNode*>& OnStack,
            TVector<CEdGraphNode*>& OutOrder, FCompileContext& Context)
        {
            if (Visited.find(Node) != Visited.end())
            {
                return true;
            }

            if (OnStack.find(Node) != OnStack.end())
            {
                Context.Error("Cycle in the graph at node '" + Node->GetNodeTitleText() + "'.");
                return false;
            }

            OnStack.insert(Node);

            for (CEdNodeGraphPin* InputPin : Node->GetInputPins())
            {
                CEdNodeGraphPin* Source = ResolveSourcePin(InputPin);
                if (Source == nullptr || Source->GetOwningNode() == nullptr)
                {
                    continue;
                }

                if (!VisitNode(Source->GetOwningNode(), Visited, OnStack, OutOrder, Context))
                {
                    return false;
                }
            }

            OnStack.erase(Node);
            Visited.insert(Node);
            OutOrder.push_back(Node);
            return true;
        }

        /** Slot an input pin reads from, either its producer's slot or a fresh one holding its literal. */
        uint16 ResolveInputSlot(CEdNodeGraphPin* InputPin, FCompileContext& Context)
        {
            if (CEdNodeGraphPin* Source = ResolveSourcePin(InputPin))
            {
                auto Found = Context.PinSlots.find(Source);
                if (Found != Context.PinSlots.end())
                {
                    return Found->second;
                }
            }

            CAudioGraphPin* AudioPin = Cast<CAudioGraphPin>(InputPin);
            if (AudioPin == nullptr)
            {
                return kAudioGraphInvalidSlot;
            }

            const EAudioGraphType Type = AudioPin->GetPinType();

            FAudioGraphSlotInit Init;
            Init.Type = Type;
            Init.Slot = Context.Allocate(Type);

            switch (Type)
            {
            case EAudioGraphType::Float:
                Init.FloatValue = AudioPin->ReadFloatDefault();
                break;

            case EAudioGraphType::Int32:
                Init.IntValue = AudioPin->ReadIntDefault();
                break;

            case EAudioGraphType::Bool:
                Init.BoolValue = AudioPin->ReadBoolDefault();
                break;

            case EAudioGraphType::Wave:
                if (CAudioStream* Wave = Cast<CAudioStream>(AudioPin->ReadObjectDefault()))
                {
                    Init.WaveIndex = Context.FindOrAddWave(Wave);
                }
                break;

            default:
                break;
            }

            Context.Result->Program.SlotInits.push_back(Init);
            return Init.Slot;
        }

        /** Pins must match the operator's ABI, or its Execute would index a neighbor's slot. */
        bool ValidateAgainstOperator(CAudioGraphNode* Node, const FAudioGraphNodeClass& Operator, FCompileContext& Context)
        {
            const TVector<TObjectPtr<CEdNodeGraphPin>>& Inputs = Node->GetInputPins();
            const TVector<TObjectPtr<CEdNodeGraphPin>>& Outputs = Node->GetOutputPins();

            const FString Label = Node->GetNodeTitleText();

            if (Inputs.size() != Operator.Signature.Inputs.size() || Outputs.size() != Operator.Signature.Outputs.size())
            {
                Context.Error("Node '" + Label + "' declares " + Format("{}", (uint32)Inputs.size()) + " inputs and "
                    + Format("{}", (uint32)Outputs.size()) + " outputs, but operator '" + Operator.Name.c_str()
                    + "' takes " + Format("{}", (uint32)Operator.Signature.Inputs.size()) + " and "
                    + Format("{}", (uint32)Operator.Signature.Outputs.size()) + ".");
                return false;
            }

            for (size_t Index = 0; Index < Inputs.size(); ++Index)
            {
                const CAudioGraphPin* Pin = Cast<CAudioGraphPin>(Inputs[Index].Get());
                if (Pin == nullptr || Pin->GetPinType() != Operator.Signature.Inputs[Index])
                {
                    Context.Error("Node '" + Label + "' input " + Format("{}", (uint32)Index)
                        + " does not match the type operator '" + Operator.Name.c_str() + "' expects.");
                    return false;
                }
            }

            for (size_t Index = 0; Index < Outputs.size(); ++Index)
            {
                const CAudioGraphPin* Pin = Cast<CAudioGraphPin>(Outputs[Index].Get());
                if (Pin == nullptr || Pin->GetPinType() != Operator.Signature.Outputs[Index])
                {
                    Context.Error("Node '" + Label + "' output " + Format("{}", (uint32)Index)
                        + " does not match the type operator '" + Operator.Name.c_str() + "' expects.");
                    return false;
                }
            }

            return true;
        }
    }

    FAudioGraphCompileResult FAudioGraphCompiler::Compile(CAudioNodeGraph* Graph)
    {
        FAudioGraphCompileResult Result;

        if (Graph == nullptr)
        {
            Result.Errors.push_back("No graph to compile.");
            return Result;
        }

        CAudioGraphOutputNode* OutputNode = Graph->FindOutputNode();
        if (OutputNode == nullptr)
        {
            Result.Errors.push_back("The graph has no Output node.");
            return Result;
        }

        FCompileContext Context;
        Context.Result = &Result;

        Result.Program.Reset();

        THashSet<CEdGraphNode*> Visited;
        THashSet<CEdGraphNode*> OnStack;
        TVector<CEdGraphNode*>  Order;

        if (!VisitNode(OutputNode, Visited, OnStack, Order, Context))
        {
            return Result;
        }

        for (CEdGraphNode* Node : Order)
        {
            if (Node->IsRerouteNode())
            {
                continue;
            }

            if (CAudioGraphInputNode* InputNode = Cast<CAudioGraphInputNode>(Node))
            {
                const TVector<TObjectPtr<CEdNodeGraphPin>>& OutputPins = InputNode->GetOutputPins();
                if (OutputPins.empty())
                {
                    continue;
                }

                if (InputNode->Type == EAudioGraphType::Wave)
                {
                    Context.Error("Graph input '" + FString(InputNode->ParameterName.c_str())
                        + "' is a Wave. Assign the wave on the Wave Player instead; gameplay cannot swap one on"
                          " a playing voice, because decoding is not safe on the audio thread.");
                    continue;
                }

                const uint16 Slot = Context.Allocate(InputNode->Type);
                Context.PinSlots[OutputPins[0]] = Slot;

                if (Result.Program.FindInput(InputNode->ParameterName) != nullptr)
                {
                    Context.Error("Two graph inputs are named '" + FString(InputNode->ParameterName.c_str()) + "'.");
                    continue;
                }

                FAudioGraphParameterDecl Decl;
                Decl.Name         = InputNode->ParameterName;
                Decl.Type         = InputNode->Type;
                Decl.Slot         = Slot;
                Decl.DefaultFloat = InputNode->DefaultFloat;
                Decl.DefaultInt   = InputNode->DefaultInt;
                Decl.DefaultBool  = InputNode->DefaultBool;
                Result.Program.Inputs.push_back(Decl);

                FAudioGraphSlotInit Init;
                Init.Type       = InputNode->Type;
                Init.Slot       = Slot;
                Init.FloatValue = InputNode->DefaultFloat;
                Init.IntValue   = InputNode->DefaultInt;
                Init.BoolValue  = InputNode->DefaultBool;
                Result.Program.SlotInits.push_back(Init);
                continue;
            }

            if (CAudioGraphNamedOutputNode* NamedOutput = Cast<CAudioGraphNamedOutputNode>(Node))
            {
                const TVector<TObjectPtr<CEdNodeGraphPin>>& InputPins = NamedOutput->GetInputPins();
                if (InputPins.empty())
                {
                    continue;
                }

                FAudioGraphParameterDecl Decl;
                Decl.Name = NamedOutput->ParameterName;
                Decl.Type = EAudioGraphType::Float;
                Decl.Slot = ResolveInputSlot(InputPins[0], Context);
                Result.Program.Outputs.push_back(Decl);
                continue;
            }

            if (CAudioGraphTriggerOutputNode* TriggerOutput = Cast<CAudioGraphTriggerOutputNode>(Node))
            {
                const TVector<TObjectPtr<CEdNodeGraphPin>>& InputPins = TriggerOutput->GetInputPins();
                if (InputPins.empty())
                {
                    continue;
                }

                FAudioGraphParameterDecl Decl;
                Decl.Name = TriggerOutput->ParameterName;
                Decl.Type = EAudioGraphType::Trigger;
                Decl.Slot = ResolveInputSlot(InputPins[0], Context);
                Result.Program.Outputs.push_back(Decl);
                continue;
            }

            if (Cast<CAudioGraphOutputNode>(Node) != nullptr)
            {
                const TVector<TObjectPtr<CEdNodeGraphPin>>& InputPins = Node->GetInputPins();

                if (InputPins.size() >= 3)
                {
                    Result.Program.OutputLeftSlot  = ResolveInputSlot(InputPins[0], Context);
                    Result.Program.OutputRightSlot = ResolveInputSlot(InputPins[1], Context);

                    // Only a wired finish pin can retire the voice; an unwired one would never fire.
                    Result.Program.FinishedSlot = InputPins[2]->HasConnection()
                        ? ResolveInputSlot(InputPins[2], Context)
                        : kAudioGraphInvalidSlot;
                }
                continue;
            }

            CAudioGraphNode* OperatorNode = Cast<CAudioGraphNode>(Node);
            if (OperatorNode == nullptr)
            {
                Context.Error("Node '" + Node->GetNodeTitleText() + "' is not an audio graph node.");
                continue;
            }

            const FName OperatorName = OperatorNode->GetOperatorName();
            const FAudioGraphNodeClass* Operator = FAudioGraphNodeRegistry::Get().Find(OperatorName);

            if (Operator == nullptr)
            {
                Context.Error("Node '" + OperatorNode->GetNodeTitleText() + "' names operator '"
                    + OperatorName.c_str() + "', which this build does not register.");
                continue;
            }

            if (!ValidateAgainstOperator(OperatorNode, *Operator, Context))
            {
                continue;
            }

            FAudioGraphNodeInstance Instance;
            Instance.OperatorName = OperatorName;
            Instance.SourceNodeID = Node->GetNodeID();

            const TVector<TObjectPtr<CEdNodeGraphPin>>& InputPins = Node->GetInputPins();
            Instance.InputSlots.reserve(InputPins.size());
            for (const TObjectPtr<CEdNodeGraphPin>& InputPin : InputPins)
            {
                Instance.InputSlots.push_back(ResolveInputSlot(InputPin.Get(), Context));
            }

            const TVector<TObjectPtr<CEdNodeGraphPin>>& OutputPins = Node->GetOutputPins();
            Instance.OutputSlots.reserve(OutputPins.size());
            for (const TObjectPtr<CEdNodeGraphPin>& OutputPin : OutputPins)
            {
                const CAudioGraphPin* Pin = Cast<CAudioGraphPin>(OutputPin.Get());
                const uint16 Slot = Pin != nullptr ? Context.Allocate(Pin->GetPinType()) : kAudioGraphInvalidSlot;

                Context.PinSlots[OutputPin.Get()] = Slot;
                Instance.OutputSlots.push_back(Slot);
            }

            Result.Program.Nodes.push_back(Move(Instance));
        }

        // Every graph answers to OnPlay, whether or not the author placed a node for it.
        if (Result.Program.FindInput(FName(kAudioGraphOnPlayInput)) == nullptr)
        {
            FAudioGraphParameterDecl Decl;
            Decl.Name = FName(kAudioGraphOnPlayInput);
            Decl.Type = EAudioGraphType::Trigger;
            Decl.Slot = Context.Allocate(EAudioGraphType::Trigger);
            Result.Program.Inputs.push_back(Decl);
        }

        if (Result.Program.Nodes.empty())
        {
            Result.Errors.push_back("Nothing is wired to the Output node.");
        }

        if (Result.Program.OutputLeftSlot == kAudioGraphInvalidSlot
            && Result.Program.OutputRightSlot == kAudioGraphInvalidSlot)
        {
            Result.Warnings.push_back("Neither output channel is wired; the graph renders silence.");
        }

        Result.bSuccess = Result.Errors.empty();
        return Result;
    }
}
