#include "RuntimePCH.h"
#include "AudioGraphInstance.h"

#include "Core/Math/Scalar.h"
#include "Log/Log.h"
#include "Memory/Memory.h"
#include "Memory/MemoryTracking.h"

namespace Lumina
{
    // Sized for a frame of parameter writes; the audio thread drains it at the start of every block.
    static constexpr uint32 kParamQueueCapacity = 512;

    FAudioGraphInstance::FAudioGraphInstance()
    {
        for (uint32 Index = 0; Index < MaxPublishedOutputs; ++Index)
        {
            PublishedOutputs[Index].store(0.0f, Atomic::MemoryOrderRelaxed);
            PublishedTriggerCounts[Index].store(0, Atomic::MemoryOrderRelaxed);
        }
    }

    FAudioGraphInstance::~FAudioGraphInstance() = default;

    bool FAudioGraphInstance::Initialize(const FAudioGraphProgram& Program,
        const TVector<TSharedPtr<FAudioGraphWaveResource>>& InWaves,
        uint32 InSampleRate, uint32 InNumChannels)
    {
        LUMINA_MEMORY_SCOPE("Audio");

        if (!Program.IsValid())
        {
            LOG_WARN("AudioGraph: refusing to instantiate an invalid program");
            return false;
        }

        SampleRate  = InSampleRate != 0 ? InSampleRate : 48000;
        NumChannels = InNumChannels != 0 ? InNumChannels : 2;

        Waves      = InWaves;
        SlotInits  = Program.SlotInits;
        InputDecls = Program.Inputs;
        OutputDecls = Program.Outputs;

        OutputLeftSlot  = Program.OutputLeftSlot;
        OutputRightSlot = Program.OutputRightSlot;
        FinishedSlot    = Program.FinishedSlot;

        OnPlaySlot = kAudioGraphInvalidSlot;
        if (const FAudioGraphParameterDecl* OnPlay = Program.FindInput(FName(kAudioGraphOnPlayInput)))
        {
            OnPlaySlot = OnPlay->Slot;
        }

        Arena.Initialize(Program.SlotCounts, kAudioGraphBlockFrames);

        Operators.clear();
        Operators.reserve(Program.Nodes.size());

        for (const FAudioGraphNodeInstance& Node : Program.Nodes)
        {
            FAudioGraphOperatorBuildParams Params;
            Params.Node        = &Node;
            Params.Arena       = &Arena;
            Params.SampleRate  = SampleRate;
            Params.BlockFrames = kAudioGraphBlockFrames;

            TUniquePtr<IAudioGraphOperator> Operator = AudioGraph::CreateOperator(Params);
            if (!Operator)
            {
                LOG_ERROR("AudioGraph: failed to build operator '{}'", Node.OperatorName.c_str());
                Operators.clear();
                return false;
            }

            Operators.push_back(Move(Operator));
        }

        ParamQueue.Initialize(kParamQueueCapacity);
        bInitialized = true;

        ApplyInitialValues();
        return true;
    }

    void FAudioGraphInstance::ApplyInitialValues()
    {
        for (const FAudioGraphSlotInit& Init : SlotInits)
        {
            switch (Init.Type)
            {
            case EAudioGraphType::Float:
                *Arena.FloatWrite(Init.Slot) = Init.FloatValue;
                break;

            case EAudioGraphType::Int32:
                *Arena.IntWrite(Init.Slot) = Init.IntValue;
                break;

            case EAudioGraphType::Bool:
                *Arena.BoolWrite(Init.Slot) = Init.BoolValue ? 1 : 0;
                break;

            case EAudioGraphType::Wave:
                {
                    const FAudioGraphWaveResource* Resource = nullptr;
                    if (Init.WaveIndex >= 0 && (size_t)Init.WaveIndex < Waves.size() && Waves[Init.WaveIndex])
                    {
                        Resource = Waves[Init.WaveIndex].get();
                    }
                    *Arena.WaveWrite(Init.Slot) = Resource;
                }
                break;

            default:
                break;
            }
        }
    }

    void FAudioGraphInstance::Rewind()
    {
        for (uint32 Index = 0; Index < MaxPublishedOutputs; ++Index)
        {
            PublishedTriggerCounts[Index].store(0, Atomic::MemoryOrderRelaxed);
        }

        for (TUniquePtr<IAudioGraphOperator>& Operator : Operators)
        {
            Operator->Reset();
        }

        ApplyInitialValues();

        BlockCursor    = kAudioGraphBlockFrames;
        BlockStartTime = 0.0;
        bStarted       = false;
        bFinished.store(false, Atomic::MemoryOrderRelease);
        RenderedFrames.store(0, Atomic::MemoryOrderRelaxed);
    }

    void FAudioGraphInstance::ApplyPendingParameters()
    {
        FAudioGraphParamCommand Command;
        uint32 Applied = 0;

        // Bounded so a flood of writes cannot stretch one block past its deadline.
        constexpr uint32 MaxCommandsPerBlock = 64;

        while (Applied < MaxCommandsPerBlock && ParamQueue.TryDequeue(Command))
        {
            ++Applied;

            switch (Command.Type)
            {
            case EAudioGraphType::Float:
                *Arena.FloatWrite(Command.Slot) = Command.FloatValue;
                break;

            case EAudioGraphType::Int32:
                *Arena.IntWrite(Command.Slot) = Command.IntValue;
                break;

            case EAudioGraphType::Bool:
                *Arena.BoolWrite(Command.Slot) = Command.BoolValue ? 1 : 0;
                break;

            case EAudioGraphType::Trigger:
                Arena.TriggerWrite(Command.Slot)->Add(0);
                break;

            default:
                break;
            }
        }
    }

    void FAudioGraphInstance::PublishOutputs()
    {
        const uint32 Count = Math::Min((uint32)OutputDecls.size(), MaxPublishedOutputs);

        for (uint32 Index = 0; Index < Count; ++Index)
        {
            if (OutputDecls[Index].Type == EAudioGraphType::Float)
            {
                PublishedOutputs[Index].store(*Arena.FloatRead(OutputDecls[Index].Slot), Atomic::MemoryOrderRelaxed);
                continue;
            }

            if (OutputDecls[Index].Type == EAudioGraphType::Trigger)
            {
                const uint32 Fired = Arena.TriggerRead(OutputDecls[Index].Slot)->Count;
                if (Fired != 0)
                {
                    const uint32 Previous = PublishedTriggerCounts[Index].load(Atomic::MemoryOrderRelaxed);
                    PublishedTriggerCounts[Index].store(Previous + Fired, Atomic::MemoryOrderRelease);
                }
            }
        }
    }

    void FAudioGraphInstance::RenderBlock()
    {
        Arena.BeginBlock();
        ApplyPendingParameters();

        if (!bStarted)
        {
            bStarted = true;
            if (OnPlaySlot != kAudioGraphInvalidSlot)
            {
                Arena.TriggerWrite(OnPlaySlot)->Add(0);
            }
        }

        FAudioGraphBlockContext Context;
        Context.NumFrames         = kAudioGraphBlockFrames;
        Context.SampleRate        = SampleRate;
        Context.InverseSampleRate = 1.0f / (float)SampleRate;
        Context.BlockStartTime    = BlockStartTime;

        for (TUniquePtr<IAudioGraphOperator>& Operator : Operators)
        {
            Operator->Execute(Context);
        }

        if (FinishedSlot != kAudioGraphInvalidSlot && !Arena.TriggerRead(FinishedSlot)->IsEmpty())
        {
            bFinished.store(true, Atomic::MemoryOrderRelease);
        }

        PublishOutputs();

        BlockStartTime += (double)kAudioGraphBlockFrames / (double)SampleRate;
    }

    void FAudioGraphInstance::Render(float* OutInterleaved, uint32 NumFrames)
    {
        if (OutInterleaved == nullptr || NumFrames == 0)
        {
            return;
        }

        if (!bInitialized)
        {
            Memory::Memset(OutInterleaved, 0, (size_t)NumFrames * NumChannels * sizeof(float));
            return;
        }

        uint32 Written = 0;

        while (Written < NumFrames)
        {
            if (BlockCursor >= kAudioGraphBlockFrames)
            {
                RenderBlock();
                BlockCursor = 0;
            }

            const uint32 Copy = Math::Min(NumFrames - Written, kAudioGraphBlockFrames - BlockCursor);

            const float* Left  = Arena.AudioRead(OutputLeftSlot) + BlockCursor;
            const float* Right = OutputRightSlot != kAudioGraphInvalidSlot
                ? Arena.AudioRead(OutputRightSlot) + BlockCursor
                : Left;

            float* Destination = OutInterleaved + (size_t)Written * NumChannels;

            if (NumChannels == 1)
            {
                for (uint32 Frame = 0; Frame < Copy; ++Frame)
                {
                    Destination[Frame] = 0.5f * (Left[Frame] + Right[Frame]);
                }
            }
            else if (NumChannels == 2)
            {
                for (uint32 Frame = 0; Frame < Copy; ++Frame)
                {
                    Destination[Frame * 2 + 0] = Left[Frame];
                    Destination[Frame * 2 + 1] = Right[Frame];
                }
            }
            else
            {
                for (uint32 Frame = 0; Frame < Copy; ++Frame)
                {
                    float* Out = Destination + (size_t)Frame * NumChannels;
                    Out[0] = Left[Frame];
                    Out[1] = Right[Frame];
                    for (uint32 Channel = 2; Channel < NumChannels; ++Channel)
                    {
                        Out[Channel] = 0.0f;
                    }
                }
            }

            Written     += Copy;
            BlockCursor += Copy;
        }

        RenderedFrames.fetch_add(NumFrames, Atomic::MemoryOrderRelaxed);
    }

    const FAudioGraphParameterDecl* FAudioGraphInstance::FindInputDecl(const FName& Name) const
    {
        for (const FAudioGraphParameterDecl& Decl : InputDecls)
        {
            if (Decl.Name == Name)
            {
                return &Decl;
            }
        }
        return nullptr;
    }

    bool FAudioGraphInstance::Enqueue(const FName& Name, EAudioGraphType Type, float FloatValue, int32 IntValue, bool BoolValue)
    {
        const FAudioGraphParameterDecl* Decl = FindInputDecl(Name);
        if (Decl == nullptr || Decl->Type != Type)
        {
            return false;
        }

        FAudioGraphParamCommand Command;
        Command.Slot       = Decl->Slot;
        Command.Type       = Type;
        Command.FloatValue = FloatValue;
        Command.IntValue   = IntValue;
        Command.BoolValue  = BoolValue;

        // Dropped rather than spun on, because a stuck parameter write would stall the caller behind audio.
        if (!ParamQueue.TryEnqueue(Command))
        {
            LOG_WARN_ONCE("AudioGraph: the parameter queue is full; dropping a parameter write");
            return false;
        }

        return true;
    }

    bool FAudioGraphInstance::SetFloatParameter(const FName& Name, float Value)
    {
        return Enqueue(Name, EAudioGraphType::Float, Value, 0, false);
    }

    bool FAudioGraphInstance::SetIntParameter(const FName& Name, int32 Value)
    {
        return Enqueue(Name, EAudioGraphType::Int32, 0.0f, Value, false);
    }

    bool FAudioGraphInstance::SetBoolParameter(const FName& Name, bool Value)
    {
        return Enqueue(Name, EAudioGraphType::Bool, 0.0f, 0, Value);
    }

    bool FAudioGraphInstance::TriggerParameter(const FName& Name)
    {
        return Enqueue(Name, EAudioGraphType::Trigger, 0.0f, 0, false);
    }

    float FAudioGraphInstance::GetFloatOutput(const FName& Name) const
    {
        const uint32 Count = Math::Min((uint32)OutputDecls.size(), MaxPublishedOutputs);

        for (uint32 Index = 0; Index < Count; ++Index)
        {
            if (OutputDecls[Index].Name == Name)
            {
                return PublishedOutputs[Index].load(Atomic::MemoryOrderRelaxed);
            }
        }

        return 0.0f;
    }

    uint32 FAudioGraphInstance::GetTriggerOutputCount(const FName& Name) const
    {
        const uint32 Count = Math::Min((uint32)OutputDecls.size(), MaxPublishedOutputs);

        for (uint32 Index = 0; Index < Count; ++Index)
        {
            if (OutputDecls[Index].Name == Name && OutputDecls[Index].Type == EAudioGraphType::Trigger)
            {
                return PublishedTriggerCounts[Index].load(Atomic::MemoryOrderAcquire);
            }
        }

        return 0;
    }
}
