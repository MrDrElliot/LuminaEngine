#pragma once

#include "Audio/Graph/AudioGraphInstance.h"
#include "Audio/Graph/AudioGraphProgram.h"
#include "Audio/Graph/AudioGraphTypes.h"
#include "Containers/Vector.h"
#include "Core/Object/Object.h"
#include "SoundBase.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Memory/SmartPtr.h"
#include "AudioGraph.generated.h"

namespace Lumina
{
    class CAudioStream;

    /** Node based sound asset, compiled by the editor into a program the mixer renders block by block. */
    REFLECT()
    class RUNTIME_API CAudioGraph : public CSoundBase
    {
        GENERATED_BODY()

    public:

        void Serialize(FArchive& Ar) override;

        bool IsPlayable() const override { return IsCompiled(); }

        bool IsCompiled() const { return Program.IsValid(); }

        // True when nothing is wired to the Output node's On Finished pin, so the graph plays until stopped.
        bool IsEndless() const { return Program.FinishedSlot == kAudioGraphInvalidSlot; }

        const FAudioGraphProgram& GetProgram() const { return Program; }

        /** Replaces the compiled program. The editor compiler owns this; nothing else should call it. */
        void SetProgram(FAudioGraphProgram&& InProgram, TVector<TObjectPtr<CAudioStream>>&& InWaves);

        /** Builds a playable copy. Call from the game thread; wave decoding happens on the first call. */
        TSharedPtr<FAudioGraphInstance> CreateInstance(uint32 SampleRate, uint32 NumChannels);

        /** Drops decoded PCM, so the next CreateInstance decodes again. */
        void ReleaseDecodedWaves();

        const TVector<FAudioGraphParameterDecl>& GetInputs() const { return Program.Inputs; }
        const TVector<FAudioGraphParameterDecl>& GetOutputs() const { return Program.Outputs; }

        /** Wave assets the compiled program addresses by index. Filled by the compiler. */
        PROPERTY()
        TVector<TObjectPtr<CAudioStream>> ReferencedWaves;

        /** Shown in the asset tooltip and the sound picker. */
        PROPERTY(Editable, Category = "Audio Graph")
        FString Description;

    private:

        void EnsureWavesDecoded();

        FAudioGraphProgram Program;

        TVector<TSharedPtr<FAudioGraphWaveResource>> DecodedWaves;
        bool bWavesDecoded = false;
    };
}
