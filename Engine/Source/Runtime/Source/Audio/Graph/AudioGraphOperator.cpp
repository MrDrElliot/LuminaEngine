#include "RuntimePCH.h"
#include "AudioGraphOperator.h"

#include "Log/Log.h"

namespace Lumina
{
    void FAudioGraphArena::Initialize(const FAudioGraphSlotCounts& Counts, uint32 InBlockFrames)
    {
        BlockFrames  = InBlockFrames == 0 ? kAudioGraphBlockFrames : InBlockFrames;
        AudioCount   = Counts.Get(EAudioGraphType::Audio);
        FloatCount   = Counts.Get(EAudioGraphType::Float);
        IntCount     = Counts.Get(EAudioGraphType::Int32);
        BoolCount    = Counts.Get(EAudioGraphType::Bool);
        TriggerCount = Counts.Get(EAudioGraphType::Trigger);
        WaveCount    = Counts.Get(EAudioGraphType::Wave);

        // Two spare elements per type back the read null and the write discard.
        AudioStorage.SetNum(IntCastChecked<int32>(static_cast<int64>((AudioCount + 2)) * BlockFrames), 0.0f);
        FloatStorage.SetNum(FloatCount + 2, 0.0f);
        IntStorage.SetNum(IntCount + 2, 0);
        BoolStorage.SetNum(BoolCount + 2, 0);
        TriggerStorage.SetNum(TriggerCount + 2);
        WaveStorage.SetNum(WaveCount + 2, nullptr);
    }

    void FAudioGraphArena::BeginBlock()
    {
        for (FAudioGraphTriggerBuffer& Buffer : TriggerStorage)
        {
            Buffer.Reset();
        }
    }

    uint32 FAudioGraphOperatorBuildParams::NumInputs() const
    {
        return (uint32)Node->InputSlots.Num();
    }

    // A pin past the compiled list reads zero and writes nowhere, so an operator may gain pins after a graph was saved.
    uint16 FAudioGraphOperatorBuildParams::InputSlot(uint32 Pin) const
    {
        return Pin < static_cast<uint32>(Node->InputSlots.Num()) ? Node->InputSlots[Pin] : kAudioGraphInvalidSlot;
    }

    uint16 FAudioGraphOperatorBuildParams::OutputSlot(uint32 Pin) const
    {
        return Pin < static_cast<uint32>(Node->OutputSlots.Num()) ? Node->OutputSlots[Pin] : kAudioGraphInvalidSlot;
    }

    FAudioGraphNodeRegistry& FAudioGraphNodeRegistry::Get()
    {
        static FAudioGraphNodeRegistry Registry;
        return Registry;
    }

    void FAudioGraphNodeRegistry::Register(FAudioGraphNodeClass&& NodeClass)
    {
        if (NodeClass.Name.IsNone() || !NodeClass.Factory)
        {
            LOG_ERROR("AudioGraph: refusing a node class with no name or no factory");
            return;
        }

        if (Find(NodeClass.Name) != nullptr)
        {
            LOG_ERROR("AudioGraph: node class '{}' is already registered", *NodeClass.Name);
            return;
        }

        Classes.Add(Move(NodeClass));
    }

    const FAudioGraphNodeClass* FAudioGraphNodeRegistry::Find(const FName& Name) const
    {
        for (const FAudioGraphNodeClass& Class : Classes)
        {
            if (Class.Name == Name)
            {
                return &Class;
            }
        }
        return nullptr;
    }

    TUniquePtr<IAudioGraphOperator> AudioGraph::CreateOperator(const FAudioGraphOperatorBuildParams& Params)
    {
        if (Params.Node == nullptr || Params.Arena == nullptr)
        {
            return nullptr;
        }

        const FAudioGraphNodeClass* NodeClass = FAudioGraphNodeRegistry::Get().Find(Params.Node->OperatorName);
        if (NodeClass == nullptr)
        {
            LOG_WARN("AudioGraph: no operator named '{}'", *Params.Node->OperatorName);
            return nullptr;
        }

        return NodeClass->Factory(Params);
    }

    FAudioGraphNodeRegistrar::FAudioGraphNodeRegistrar(FAudioGraphNodeClass&& NodeClass)
    {
        FAudioGraphNodeRegistry::Get().Register(Move(NodeClass));
    }
}
