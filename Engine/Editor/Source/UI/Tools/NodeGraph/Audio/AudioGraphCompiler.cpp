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
                Result->Errors.Add(Message);
            }

            // Deduplicated, because each entry costs a full PCM decode held for the life of the voice.
            int32 FindOrAddWave(CAudioStream* Wave)
            {
                for (int32 Index = 0; Index < Result->Waves.Num(); ++Index)
                {
                    if (Result->Waves[Index] == Wave)
                    {
                        return Index;
                    }
                }

                Result->Waves.Add(Wave);
                return Result->Waves.Num() - 1;
            }
        };

        /** Depth first post order over the input closure, so a producer always lands before its consumer. */
        bool VisitNode(CEdGraphNode* Node, THashSet<CEdGraphNode*>& Visited, THashSet<CEdGraphNode*>& OnStack,
            TVector<CEdGraphNode*>& OutOrder, FCompileContext& Context)
        {
            if (Visited.Contains(Node))
            {
                return true;
            }

            if (OnStack.Contains(Node))
            {
                Context.Error("Cycle in the graph at node '" + Node->GetNodeTitleText() + "'.");
                return false;
            }

            OnStack.Add(Node);

            for (const auto& InputPinRef : Node->GetInputPins())
            {
                CEdNodeGraphPin* InputPin = InputPinRef.Get();
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

            OnStack.Remove(Node);
            Visited.Add(Node);
            OutOrder.Add(Node);
            return true;
        }

        /** Slot an input pin reads from, either its producer's slot or a fresh one holding its literal. */
        uint16 ResolveInputSlot(CEdNodeGraphPin* InputPin, FCompileContext& Context)
        {
            if (CEdNodeGraphPin* Source = ResolveSourcePin(InputPin))
            {
                auto Found = Context.PinSlots.Find(Source);
                if (Found != nullptr)
                {
                    return (*Found);
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

            Context.Result->Program.SlotInits.Add(Init);
            return Init.Slot;
        }

        /** Pins must match the operator's ABI, or its Execute would index a neighbor's slot. */
        bool ValidateAgainstOperator(CAudioGraphNode* Node, const FAudioGraphNodeClass& Operator, FCompileContext& Context)
        {
            const TVector<TStrongObjectPtr<CEdNodeGraphPin>>& Inputs = Node->GetInputPins();
            const TVector<TStrongObjectPtr<CEdNodeGraphPin>>& Outputs = Node->GetOutputPins();

            const FString Label = Node->GetNodeTitleText();

            if (Inputs.Num() != Operator.Signature.Inputs.Num() || Outputs.Num() != Operator.Signature.Outputs.Num())
            {
                Context.Error("Node '" + Label + "' declares " + Format("{}", (uint32)Inputs.Num()) + " inputs and "
                    + Format("{}", (uint32)Outputs.Num()) + " outputs, but operator '" + *Operator.Name
                    + "' takes " + Format("{}", (uint32)Operator.Signature.Inputs.Num()) + " and "
                    + Format("{}", (uint32)Operator.Signature.Outputs.Num()) + ".");
                return false;
            }

            for (int32 Index = 0; Index < Inputs.Num(); ++Index)
            {
                const CAudioGraphPin* Pin = Cast<CAudioGraphPin>(Inputs[Index].Get());
                if (Pin == nullptr || Pin->GetPinType() != Operator.Signature.Inputs[Index])
                {
                    Context.Error("Node '" + Label + "' input " + Format("{}", (uint32)Index)
                        + " does not match the type operator '" + *Operator.Name + "' expects.");
                    return false;
                }
            }

            for (int32 Index = 0; Index < Outputs.Num(); ++Index)
            {
                const CAudioGraphPin* Pin = Cast<CAudioGraphPin>(Outputs[Index].Get());
                if (Pin == nullptr || Pin->GetPinType() != Operator.Signature.Outputs[Index])
                {
                    Context.Error("Node '" + Label + "' output " + Format("{}", (uint32)Index)
                        + " does not match the type operator '" + *Operator.Name + "' expects.");
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
            Result.Errors.Add("No graph to compile.");
            return Result;
        }

        CAudioGraphOutputNode* OutputNode = Graph->FindOutputNode();
        if (OutputNode == nullptr)
        {
            Result.Errors.Add("The graph has no Output node.");
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
                const TVector<TStrongObjectPtr<CEdNodeGraphPin>>& OutputPins = InputNode->GetOutputPins();
                if (OutputPins.IsEmpty())
                {
                    continue;
                }

                if (InputNode->Type == EAudioGraphType::Wave)
                {
                    Context.Error("Graph input '" + FString(*InputNode->ParameterName)
                        + "' is a Wave. Assign the wave on the Wave Player instead; gameplay cannot swap one on"
                          " a playing voice, because decoding is not safe on the audio thread.");
                    continue;
                }

                const uint16 Slot = Context.Allocate(InputNode->Type);
                Context.PinSlots[OutputPins[0].Get()] = Slot;

                if (Result.Program.FindInput(InputNode->ParameterName) != nullptr)
                {
                    Context.Error("Two graph inputs are named '" + FString(*InputNode->ParameterName) + "'.");
                    continue;
                }

                FAudioGraphParameterDecl Decl;
                Decl.Name         = InputNode->ParameterName;
                Decl.Type         = InputNode->Type;
                Decl.Slot         = Slot;
                Decl.DefaultFloat = InputNode->DefaultFloat;
                Decl.DefaultInt   = InputNode->DefaultInt;
                Decl.DefaultBool  = InputNode->DefaultBool;
                Result.Program.Inputs.Add(Decl);

                FAudioGraphSlotInit Init;
                Init.Type       = InputNode->Type;
                Init.Slot       = Slot;
                Init.FloatValue = InputNode->DefaultFloat;
                Init.IntValue   = InputNode->DefaultInt;
                Init.BoolValue  = InputNode->DefaultBool;
                Result.Program.SlotInits.Add(Init);
                continue;
            }

            if (CAudioGraphNamedOutputNode* NamedOutput = Cast<CAudioGraphNamedOutputNode>(Node))
            {
                const TVector<TStrongObjectPtr<CEdNodeGraphPin>>& InputPins = NamedOutput->GetInputPins();
                if (InputPins.IsEmpty())
                {
                    continue;
                }

                FAudioGraphParameterDecl Decl;
                Decl.Name = NamedOutput->ParameterName;
                Decl.Type = EAudioGraphType::Float;
                Decl.Slot = ResolveInputSlot(InputPins[0].Get(), Context);
                Result.Program.Outputs.Add(Decl);
                continue;
            }

            if (CAudioGraphTriggerOutputNode* TriggerOutput = Cast<CAudioGraphTriggerOutputNode>(Node))
            {
                const TVector<TStrongObjectPtr<CEdNodeGraphPin>>& InputPins = TriggerOutput->GetInputPins();
                if (InputPins.IsEmpty())
                {
                    continue;
                }

                FAudioGraphParameterDecl Decl;
                Decl.Name = TriggerOutput->ParameterName;
                Decl.Type = EAudioGraphType::Trigger;
                Decl.Slot = ResolveInputSlot(InputPins[0].Get(), Context);
                Result.Program.Outputs.Add(Decl);
                continue;
            }

            if (Cast<CAudioGraphOutputNode>(Node) != nullptr)
            {
                const TVector<TStrongObjectPtr<CEdNodeGraphPin>>& InputPins = Node->GetInputPins();

                if (InputPins.Num() >= 3)
                {
                    Result.Program.OutputLeftSlot  = ResolveInputSlot(InputPins[0].Get(), Context);
                    Result.Program.OutputRightSlot = ResolveInputSlot(InputPins[1].Get(), Context);

                    // Only a wired finish pin can retire the voice; an unwired one would never fire.
                    Result.Program.FinishedSlot = InputPins[2]->HasConnection()
                        ? ResolveInputSlot(InputPins[2].Get(), Context)
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
                    + *OperatorName + "', which this build does not register.");
                continue;
            }

            if (!ValidateAgainstOperator(OperatorNode, *Operator, Context))
            {
                continue;
            }

            FAudioGraphNodeInstance Instance;
            Instance.OperatorName = OperatorName;
            Instance.SourceNodeID = Node->GetNodeID();

            const TVector<TStrongObjectPtr<CEdNodeGraphPin>>& InputPins = Node->GetInputPins();
            Instance.InputSlots.Reserve(InputPins.Num());
            for (const TStrongObjectPtr<CEdNodeGraphPin>& InputPin : InputPins)
            {
                Instance.InputSlots.Add(ResolveInputSlot(InputPin.Get(), Context));
            }

            if (Operator->Signature.Variadic == EAudioGraphType::Wave)
            {
                TVector<CAudioStream*> NodeWaves;
                OperatorNode->GetNodeWaves(NodeWaves);

                for (CAudioStream* Wave : NodeWaves)
                {
                    if (Wave == nullptr)
                    {
                        continue;
                    }

                    FAudioGraphSlotInit Init;
                    Init.Type      = EAudioGraphType::Wave;
                    Init.Slot      = Context.Allocate(EAudioGraphType::Wave);
                    Init.WaveIndex = Context.FindOrAddWave(Wave);
                    Result.Program.SlotInits.Add(Init);
                    Instance.InputSlots.Add(Init.Slot);
                }

                if (Instance.InputSlots.Num() == InputPins.Num())
                {
                    Result.Warnings.Add("Node '" + OperatorNode->GetNodeTitleText() + "' has no waves, so it plays nothing.");
                }
            }

            const TVector<TStrongObjectPtr<CEdNodeGraphPin>>& OutputPins = Node->GetOutputPins();
            Instance.OutputSlots.Reserve(OutputPins.Num());
            for (const TStrongObjectPtr<CEdNodeGraphPin>& OutputPin : OutputPins)
            {
                const CAudioGraphPin* Pin = Cast<CAudioGraphPin>(OutputPin.Get());
                const uint16 Slot = Pin != nullptr ? Context.Allocate(Pin->GetPinType()) : kAudioGraphInvalidSlot;

                Context.PinSlots[OutputPin.Get()] = Slot;
                Instance.OutputSlots.Add(Slot);
            }

            Result.Program.Nodes.Add(Move(Instance));
        }

        // Every graph answers to OnPlay, whether or not the author placed a node for it.
        if (Result.Program.FindInput(FName(kAudioGraphOnPlayInput)) == nullptr)
        {
            FAudioGraphParameterDecl Decl;
            Decl.Name = FName(kAudioGraphOnPlayInput);
            Decl.Type = EAudioGraphType::Trigger;
            Decl.Slot = Context.Allocate(EAudioGraphType::Trigger);
            Result.Program.Inputs.Add(Decl);
        }

        if (Result.Program.Nodes.IsEmpty())
        {
            Result.Errors.Add("Nothing is wired to the Output node.");
        }

        if (Result.Program.OutputLeftSlot == kAudioGraphInvalidSlot
            && Result.Program.OutputRightSlot == kAudioGraphInvalidSlot)
        {
            Result.Warnings.Add("Neither output channel is wired; the graph renders silence.");
        }

        Result.bSuccess = Result.Errors.IsEmpty();
        return Result;
    }
}
