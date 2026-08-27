#include "RuntimePCH.h"
#include "AudioGlobals.h"

#include "ProceduralAudioStream.h"

namespace Lumina
{
    namespace
    {
        IAudioContext* GAudioContext;
        
        // A stand-in for a missing audio device.
        class FNullAudioContext final : public IAudioContext
        {
        public:

            FAudioHandle PlayAudio(const TSharedPtr<FAudioData>&, const FAudioPlayParams&) override { return {}; }
            FAudioHandle PlayFile(FStringView, const FAudioPlayParams&) override { return {}; }
            FAudioHandle PlayProceduralStream(TSharedPtr<FProceduralAudioStream>, const FAudioPlayParams&) override { return {}; }
            FAudioHandle PlayAudioGraph(TSharedPtr<FAudioGraphInstance>, const FAudioPlayParams&) override { return {}; }
            bool SetGraphFloatParameter(FAudioHandle, const FName&, float) override { return false; }
            bool SetGraphIntParameter(FAudioHandle, const FName&, int32) override { return false; }
            bool SetGraphBoolParameter(FAudioHandle, const FName&, bool) override { return false; }
            bool TriggerGraphParameter(FAudioHandle, const FName&) override { return false; }
            float GetGraphFloatOutput(FAudioHandle, const FName&) const override { return 0.0f; }
            uint32 GetGraphTriggerOutputCount(FAudioHandle, const FName&) const override { return 0; }

            void StopSound(FAudioHandle, EAudioStopMode, float) override {}
            void StopAllSounds(EAudioStopMode, float) override {}

            void SetVolume(FAudioHandle, float) override {}
            void SetPitch(FAudioHandle, float) override {}
            void SetLooping(FAudioHandle, bool) override {}
            void SetPosition(FAudioHandle, FVector3) override {}
            void SetVelocity(FAudioHandle, FVector3) override {}
            void SetDirection(FAudioHandle, FVector3) override {}
            void SetAttenuation(FAudioHandle, const SAudioAttenuation&) override {}
            void SetMinMaxDistance(FAudioHandle, float, float) override {}
            void SetPan(FAudioHandle, float) override {}
            void SetPaused(FAudioHandle, bool) override {}
            void SetBus(FAudioHandle, EAudioBus) override {}
            void SetPriority(FAudioHandle, uint8) override {}
            void SetOcclusion(FAudioHandle, float, float, float) override {}
            void SetLowPassCutoff(FAudioHandle, float) override {}
            void FadeTo(FAudioHandle, float, float) override {}
            void SeekToFrame(FAudioHandle, uint64) override {}

            // No voice was ever handed out, so every handle reads as one that has finished.
            EAudioVoiceState GetVoiceState(FAudioHandle) const override { return EAudioVoiceState::Free; }
            uint64 GetPlaybackFrame(FAudioHandle) const override { return 0; }
            uint32 GetActiveVoiceCount() const override { return 0; }
            uint32 GetMaxVoiceCount() const override { return 0; }
            uint64 GetDroppedVoiceCount() const override { return 0; }

            void UpdateListener(uint32, FVector3, FQuat, FVector3) override {}
            void SetListenerEnabled(uint32, bool) override {}
            uint32 GetListenerCount() const override { return 0; }

            void SetBusVolume(EAudioBus, float) override {}
            float GetBusVolume(EAudioBus) const override { return 1.0f; }
            void SetBusMuted(EAudioBus, bool) override {}
            bool IsBusMuted(EAudioBus) const override { return false; }
            void SetBusPitch(EAudioBus, float) override {}

            void SetBusReverbSend(EAudioBus, float) override {}
            float GetBusReverbSend(EAudioBus) const override { return 0.0f; }

            void SetReverbParams(const FAudioReverbParams&) override {}
            FAudioReverbParams GetReverbParams() const override { return {}; }

            void SetDopplerScale(float) override {}
            float GetDopplerScale() const override { return 1.0f; }

            void SetSuspended(bool) override {}
            bool IsSuspended() const override { return false; }

            void SetMaxVoiceCount(uint32) override {}
            void SetVolumeSmoothing(float) override {}

            void ApplyDeviceConfig(uint32, uint32, uint32) override {}
            FAudioDeviceInfo GetDeviceInfo() const override { return {}; }

            TSharedPtr<FProceduralAudioStream> CreateProceduralStream(uint32, uint32, uint32) override { return nullptr; }
        };
    }

    IAudioContext& Audio::Context()
    {
        // A polymorphic global writes its vptr during dynamic init, leaving a window this has no equivalent of.
        static FNullAudioContext NullContext;

        IAudioContext* Live = GAudioContext;
        return (Live != nullptr) ? *Live : NullContext;
    }

    bool Audio::HasDevice()
    {
        return GAudioContext != nullptr;
    }

    IAudioContext* Audio::Internal::SetContext(IAudioContext* Context)
    {
        IAudioContext* Previous = GAudioContext;
        GAudioContext = Context;
        return Previous;
    }
}
