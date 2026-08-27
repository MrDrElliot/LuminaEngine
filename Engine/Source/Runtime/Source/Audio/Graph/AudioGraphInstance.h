#pragma once

#include "AudioGraphOperator.h"
#include "AudioGraphProgram.h"
#include "AudioGraphTypes.h"
#include "Containers/Name.h"
#include "Containers/Vector.h"
#include "Core/Threading/Atomic.h"
#include "Containers/BoundedQueue.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    /** Name of the trigger input every graph gets, raised once at the start of the first block. */
    inline const char* kAudioGraphOnPlayInput = "OnPlay";

    /** Name of the trigger output a graph raises to say the voice has nothing left to play. */
    inline const char* kAudioGraphOnFinishedOutput = "OnFinished";

    /** A parameter write in flight from the game thread to the audio thread. */
    struct FAudioGraphParamCommand
    {
        uint16          Slot = kAudioGraphInvalidSlot;
        EAudioGraphType Type = EAudioGraphType::Invalid;
        float           FloatValue = 0.0f;
        int32           IntValue = 0;
        bool            BoolValue = false;
    };

    /** One playing copy of a compiled audio graph, rendered block by block on the audio device thread. */
    class RUNTIME_API FAudioGraphInstance
    {
    public:

        /** Float outputs beyond this are still computed, they are just not readable from gameplay. */
        static constexpr uint32 MaxPublishedOutputs = 16;

        FAudioGraphInstance();
        ~FAudioGraphInstance();

        FAudioGraphInstance(const FAudioGraphInstance&) = delete;
        FAudioGraphInstance& operator=(const FAudioGraphInstance&) = delete;

        /** Allocates the arena and builds one operator per node. Call off the audio thread. */
        bool Initialize(const FAudioGraphProgram& Program,
            const TVector<TSharedPtr<FAudioGraphWaveResource>>& Waves,
            uint32 InSampleRate, uint32 InNumChannels);

        bool IsInitialized() const { return bInitialized; }

        /** Renders NumFrames of interleaved output. Runs on the audio device thread. */
        void Render(float* OutInterleaved, uint32 NumFrames);

        /** Rewinds every operator and replays from the start, including the OnPlay trigger. */
        void Rewind();

        /** True once the graph raised its OnFinished output. */
        bool IsFinished() const { return bFinished.load(Atomic::MemoryOrderAcquire); }

        uint64 GetRenderedFrames() const { return RenderedFrames.load(Atomic::MemoryOrderRelaxed); }

        uint32 GetSampleRate() const { return SampleRate; }
        uint32 GetChannelCount() const { return NumChannels; }

        bool SetFloatParameter(const FName& Name, float Value);
        bool SetIntParameter(const FName& Name, int32 Value);
        bool SetBoolParameter(const FName& Name, bool Value);

        /** Queues a trigger event, raised at the start of the next block. */
        bool TriggerParameter(const FName& Name);

        /** Last block's value of a float output, or 0 when the graph declares no such output. */
        float GetFloatOutput(const FName& Name) const;

        // Monotonic, so a reader compares against its own last read and two readers cannot starve each other.
        uint32 GetTriggerOutputCount(const FName& Name) const;

        const TVector<FAudioGraphParameterDecl>& GetInputs() const { return InputDecls; }
        const TVector<FAudioGraphParameterDecl>& GetOutputs() const { return OutputDecls; }

    private:

        void RenderBlock();
        void ApplyPendingParameters();
        void ApplyInitialValues();
        void PublishOutputs();

        const FAudioGraphParameterDecl* FindInputDecl(const FName& Name) const;

        bool Enqueue(const FName& Name, EAudioGraphType Type, float FloatValue, int32 IntValue, bool BoolValue);

        FAudioGraphArena                         Arena;
        TVector<TUniquePtr<IAudioGraphOperator>> Operators;

        TVector<FAudioGraphSlotInit>             SlotInits;
        TVector<FAudioGraphParameterDecl>        InputDecls;
        TVector<FAudioGraphParameterDecl>        OutputDecls;

        /** Decoded PCM kept alive for the whole voice, so unloading the asset cannot pull it away. */
        TVector<TSharedPtr<FAudioGraphWaveResource>> Waves;

        TBoundedMPSCQueue<FAudioGraphParamCommand> ParamQueue;

        uint16 OutputLeftSlot  = kAudioGraphInvalidSlot;
        uint16 OutputRightSlot = kAudioGraphInvalidSlot;
        uint16 FinishedSlot    = kAudioGraphInvalidSlot;
        uint16 OnPlaySlot      = kAudioGraphInvalidSlot;

        uint32 SampleRate  = 48000;
        uint32 NumChannels = 2;

        /** Frames of the current block already handed to the mixer. */
        uint32 BlockCursor = kAudioGraphBlockFrames;

        double BlockStartTime = 0.0;

        TAtomic<float>  PublishedOutputs[MaxPublishedOutputs];
        TAtomic<uint32> PublishedTriggerCounts[MaxPublishedOutputs];
        TAtomic<bool>   bFinished{false};
        TAtomic<uint64> RenderedFrames{0};

        bool bInitialized = false;
        bool bStarted = false;
    };
}
