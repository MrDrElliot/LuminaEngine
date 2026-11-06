#include "RuntimePCH.h"
#include "AudioGlobals.h"
#include "AudioReverb.h"

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

            SAudioHandle PlayAudio(const TSharedPtr<FAudioData>&, const SAudioPlayParams&) override { return {}; }
            SAudioHandle PlayFile(FStringView, const SAudioPlayParams&) override { return {}; }
            SAudioHandle PlayProceduralStream(TSharedPtr<FProceduralAudioStream>, const SAudioPlayParams&) override { return {}; }
            SAudioHandle PlayAudioGraph(TSharedPtr<FAudioGraphInstance>, const SAudioPlayParams&) override { return {}; }
            bool SetGraphFloatParameter(SAudioHandle, const FName&, float) override { return false; }
            bool SetGraphIntParameter(SAudioHandle, const FName&, int32) override { return false; }
            bool SetGraphBoolParameter(SAudioHandle, const FName&, bool) override { return false; }
            bool TriggerGraphParameter(SAudioHandle, const FName&) override { return false; }
            float GetGraphFloatOutput(SAudioHandle, const FName&) const override { return 0.0f; }
            uint32 GetGraphTriggerOutputCount(SAudioHandle, const FName&) const override { return 0; }

            void StopSound(SAudioHandle, EAudioStopMode, float) override {}
            void StopAllSounds(EAudioStopMode, float) override {}

            void SetVolume(SAudioHandle, float) override {}
            void SetPitch(SAudioHandle, float) override {}
            void SetLooping(SAudioHandle, bool) override {}
            void SetPosition(SAudioHandle, FVector3) override {}
            void SetVelocity(SAudioHandle, FVector3) override {}
            void SetDirection(SAudioHandle, FVector3) override {}
            void SetAttenuation(SAudioHandle, const SAudioAttenuation&) override {}
            void SetMinMaxDistance(SAudioHandle, float, float) override {}
            void SetPan(SAudioHandle, float) override {}
            void SetPaused(SAudioHandle, bool) override {}
            void SetBus(SAudioHandle, EAudioBus) override {}
            void SetPriority(SAudioHandle, uint8) override {}
            void SetOcclusion(SAudioHandle, float, float, float) override {}
            void SetLowPassCutoff(SAudioHandle, float) override {}
            void FadeTo(SAudioHandle, float, float) override {}
            void SeekToFrame(SAudioHandle, uint64) override {}

            // No voice was ever handed out, so every handle reads as one that has finished.
            EAudioVoiceState GetVoiceState(SAudioHandle) const override { return EAudioVoiceState::Free; }
            uint64 GetPlaybackFrame(SAudioHandle) const override { return 0; }
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
