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
        AudioStorage.resize((size_t)(AudioCount + 2) * BlockFrames, 0.0f);
        FloatStorage.resize(FloatCount + 2, 0.0f);
        IntStorage.resize(IntCount + 2, 0);
        BoolStorage.resize(BoolCount + 2, 0);
        TriggerStorage.resize(TriggerCount + 2);
        WaveStorage.resize(WaveCount + 2, nullptr);
    }

    void FAudioGraphArena::BeginBlock()
    {
        for (FAudioGraphTriggerBuffer& Buffer : TriggerStorage)
        {
            Buffer.Reset();
        }
    }

    uint16 FAudioGraphOperatorBuildParams::InputSlot(uint32 Pin) const
    {
        return Pin < Node->InputSlots.size() ? Node->InputSlots[Pin] : kAudioGraphInvalidSlot;
    }

    uint16 FAudioGraphOperatorBuildParams::OutputSlot(uint32 Pin) const
    {
        return Pin < Node->OutputSlots.size() ? Node->OutputSlots[Pin] : kAudioGraphInvalidSlot;
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
            LOG_ERROR("AudioGraph: node class '{}' is already registered", NodeClass.Name.c_str());
            return;
        }

        Classes.push_back(Move(NodeClass));
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
            LOG_WARN("AudioGraph: no operator named '{}'", Params.Node->OperatorName.c_str());
            return nullptr;
        }

        // An operator indexes its pins positionally, so a short slot list would read a neighbor's value.
        if (Params.Node->InputSlots.size() < NodeClass->Signature.Inputs.size()
            || Params.Node->OutputSlots.size() < NodeClass->Signature.Outputs.size())
        {
            LOG_ERROR("AudioGraph: operator '{}' was compiled with {} inputs and {} outputs, but needs {} and {}",
                NodeClass->Name.c_str(),
                (uint32)Params.Node->InputSlots.size(), (uint32)Params.Node->OutputSlots.size(),
                (uint32)NodeClass->Signature.Inputs.size(), (uint32)NodeClass->Signature.Outputs.size());
            return nullptr;
        }

        return NodeClass->Factory(Params);
    }

    FAudioGraphNodeRegistrar::FAudioGraphNodeRegistrar(FAudioGraphNodeClass&& NodeClass)
    {
        FAudioGraphNodeRegistry::Get().Register(Move(NodeClass));
    }
}
