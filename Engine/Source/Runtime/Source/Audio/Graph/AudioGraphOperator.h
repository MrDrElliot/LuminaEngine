#pragma once

#include "AudioGraphProgram.h"
#include "AudioGraphTypes.h"
#include "Containers/Function.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{

    /** Typed storage every wire in a live graph instance points into. */
    class RUNTIME_API FAudioGraphArena
    {
    public:

        void Initialize(const FAudioGraphSlotCounts& Counts, uint32 InBlockFrames);

        /** Drops the previous block's trigger events. Audio slots are not recleared, see Execute. */
        void BeginBlock();

        uint32 GetBlockFrames() const { return BlockFrames; }

        float* AudioRead(uint16 Slot)  { return AudioStorage.data() + (size_t)ResolveRead(Slot, AudioCount) * BlockFrames; }
        float* AudioWrite(uint16 Slot) { return AudioStorage.data() + (size_t)ResolveWrite(Slot, AudioCount) * BlockFrames; }

        float* FloatRead(uint16 Slot)  { return FloatStorage.data() + ResolveRead(Slot, FloatCount); }
        float* FloatWrite(uint16 Slot) { return FloatStorage.data() + ResolveWrite(Slot, FloatCount); }

        int32* IntRead(uint16 Slot)  { return IntStorage.data() + ResolveRead(Slot, IntCount); }
        int32* IntWrite(uint16 Slot) { return IntStorage.data() + ResolveWrite(Slot, IntCount); }

        uint8* BoolRead(uint16 Slot)  { return BoolStorage.data() + ResolveRead(Slot, BoolCount); }
        uint8* BoolWrite(uint16 Slot) { return BoolStorage.data() + ResolveWrite(Slot, BoolCount); }

        FAudioGraphTriggerBuffer* TriggerRead(uint16 Slot)  { return TriggerStorage.data() + ResolveRead(Slot, TriggerCount); }
        FAudioGraphTriggerBuffer* TriggerWrite(uint16 Slot) { return TriggerStorage.data() + ResolveWrite(Slot, TriggerCount); }

        const FAudioGraphWaveResource** WaveRead(uint16 Slot)  { return WaveStorage.data() + ResolveRead(Slot, WaveCount); }
        const FAudioGraphWaveResource** WaveWrite(uint16 Slot) { return WaveStorage.data() + ResolveWrite(Slot, WaveCount); }

    private:

        // An unbound pin reads the zeroed element past the end, so no operator has to null check a wire.
        static uint32 ResolveRead(uint16 Slot, uint32 Count) { return Slot < Count ? Slot : Count; }

        // Discard element for an unbound output, one past the read null so a write cannot poison a read.
        static uint32 ResolveWrite(uint16 Slot, uint32 Count) { return Slot < Count ? Slot : Count + 1; }

        TVector<float>                    AudioStorage;
        TVector<float>                    FloatStorage;
        TVector<int32>                    IntStorage;
        TVector<uint8>                    BoolStorage;
        TVector<FAudioGraphTriggerBuffer> TriggerStorage;
        TVector<const FAudioGraphWaveResource*> WaveStorage;

        uint32 BlockFrames   = kAudioGraphBlockFrames;
        uint32 AudioCount    = 0;
        uint32 FloatCount    = 0;
        uint32 IntCount      = 0;
        uint32 BoolCount     = 0;
        uint32 TriggerCount  = 0;
        uint32 WaveCount     = 0;
    };

    /** Everything an operator needs to bind its pins at construction. */
    struct RUNTIME_API FAudioGraphOperatorBuildParams
    {
        const FAudioGraphNodeInstance*  Node       = nullptr;
        FAudioGraphArena*               Arena      = nullptr;
        uint32                          SampleRate = 48000;
        uint32                          BlockFrames = kAudioGraphBlockFrames;

        float* AudioIn(uint32 Pin) const   { return Arena->AudioRead(InputSlot(Pin)); }
        float* AudioOut(uint32 Pin) const  { return Arena->AudioWrite(OutputSlot(Pin)); }

        const float* FloatIn(uint32 Pin) const { return Arena->FloatRead(InputSlot(Pin)); }
        float* FloatOut(uint32 Pin) const      { return Arena->FloatWrite(OutputSlot(Pin)); }

        const int32* IntIn(uint32 Pin) const { return Arena->IntRead(InputSlot(Pin)); }
        int32* IntOut(uint32 Pin) const      { return Arena->IntWrite(OutputSlot(Pin)); }

        const uint8* BoolIn(uint32 Pin) const { return Arena->BoolRead(InputSlot(Pin)); }
        uint8* BoolOut(uint32 Pin) const      { return Arena->BoolWrite(OutputSlot(Pin)); }

        const FAudioGraphTriggerBuffer* TriggerIn(uint32 Pin) const { return Arena->TriggerRead(InputSlot(Pin)); }
        FAudioGraphTriggerBuffer* TriggerOut(uint32 Pin) const      { return Arena->TriggerWrite(OutputSlot(Pin)); }

        const FAudioGraphWaveResource** WaveIn(uint32 Pin) const { return Arena->WaveRead(InputSlot(Pin)); }

        uint16 InputSlot(uint32 Pin) const;
        uint16 OutputSlot(uint32 Pin) const;
    };

    /** One block of DSP for one node. Constructed once per instance, executed once per block. */
    class RUNTIME_API IAudioGraphOperator
    {
    public:

        virtual ~IAudioGraphOperator() = default;

        /** Returns the operator to its start state without reallocating. */
        virtual void Reset() {}

        /** Runs on the audio device thread, so it must not allocate, lock or block. */
        virtual void Execute(const FAudioGraphBlockContext& Context) = 0;

        // Every audio output must be written in full each block; the arena does not reclear them.
    };

    using FAudioGraphOperatorFactory = TFunction<TUniquePtr<IAudioGraphOperator>(const FAudioGraphOperatorBuildParams&)>;

    /** Pin types an operator reads and writes, in the order its Execute indexes them. */
    struct FAudioGraphOperatorSignature
    {
        TVector<EAudioGraphType> Inputs;
        TVector<EAudioGraphType> Outputs;
    };

    /** A runtime operator, addressed by the name compiled programs store. */
    struct FAudioGraphNodeClass
    {
        FName Name;

        /** The editor node declaring this operator must present pins matching it, or the compile errors. */
        FAudioGraphOperatorSignature Signature;

        FAudioGraphOperatorFactory Factory;
    };

    /** Every node type the engine and its plugins offer, keyed by the name compiled programs store. */
    class RUNTIME_API FAudioGraphNodeRegistry
    {
    public:

        static FAudioGraphNodeRegistry& Get();

        void Register(FAudioGraphNodeClass&& NodeClass);

        const FAudioGraphNodeClass* Find(const FName& Name) const;

        const TVector<FAudioGraphNodeClass>& GetAll() const { return Classes; }

    private:

        TVector<FAudioGraphNodeClass> Classes;
    };

    namespace AudioGraph
    {
        /** Builds the operator for Params.Node, or null when the program names a node this build lacks. */
        RUNTIME_API TUniquePtr<IAudioGraphOperator> CreateOperator(const FAudioGraphOperatorBuildParams& Params);
    }

    /** Declares a node class at static init; the registrar's name only has to be unique in its file. */
    struct RUNTIME_API FAudioGraphNodeRegistrar
    {
        FAudioGraphNodeRegistrar(FAudioGraphNodeClass&& NodeClass);
    };
}
