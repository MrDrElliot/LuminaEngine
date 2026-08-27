#include "RuntimePCH.h"
#ifdef LE_PLATFORM_LINUX

#include "Audio/AudioDevice.h"
#include "Containers/Vector.h"
#include "Core/Math/Scalar.h"
#include "Core/Threading/Atomic.h"
#include "Core/Threading/Thread.h"
#include "Log/Log.h"

#include <dlfcn.h>

namespace Lumina
{
	namespace
	{
		// Public ALSA ABI values, spelled out so this needs no libasound headers at build time.
		constexpr int kStreamPlayback      = 0;
		constexpr int kAccessRwInterleaved = 3;
		constexpr int kFormatFloatLe       = 14;

		constexpr uint32 kDefaultPeriodFrames = 480;
		constexpr uint32 kPeriodsPerBuffer    = 4;

		/** libasound resolved at runtime, so a machine without it loses audio rather than failing to launch. */
		struct FAlsaApi
		{
			void* Library = nullptr;

			int  (*Open)(void**, const char*, int, int) = nullptr;
			int  (*Close)(void*) = nullptr;
			int  (*ParamsMalloc)(void**) = nullptr;
			void (*ParamsFree)(void*) = nullptr;
			int  (*ParamsAny)(void*, void*) = nullptr;
			int  (*SetAccess)(void*, void*, int) = nullptr;
			int  (*SetFormat)(void*, void*, int) = nullptr;
			int  (*SetChannelsNear)(void*, void*, unsigned int*) = nullptr;
			int  (*SetRateNear)(void*, void*, unsigned int*, int*) = nullptr;
			int  (*SetPeriodSizeNear)(void*, void*, unsigned long*, int*) = nullptr;
			int  (*SetBufferSizeNear)(void*, void*, unsigned long*) = nullptr;
			int  (*ApplyParams)(void*, void*) = nullptr;
			int  (*Prepare)(void*) = nullptr;
			long (*WriteInterleaved)(void*, const void*, unsigned long) = nullptr;
			int  (*Recover)(void*, int, int) = nullptr;
			int  (*Drop)(void*) = nullptr;
			const char* (*StrError)(int) = nullptr;

			template <typename TFunction>
			bool Bind(TFunction& Target, const char* Name)
			{
				Target = (TFunction)dlsym(Library, Name);
				if (Target == nullptr)
				{
					LOG_WARN("[Audio] libasound is missing '{0}'", Name);
					return false;
				}
				return true;
			}

			bool Load()
			{
				Library = dlopen("libasound.so.2", RTLD_NOW);
				if (Library == nullptr)
				{
					Library = dlopen("libasound.so", RTLD_NOW);
				}

				if (Library == nullptr)
				{
					LOG_WARN("[Audio] libasound is not installed; there will be no audio output");
					return false;
				}

				const bool bBound =
					Bind(Open,              "snd_pcm_open") &&
					Bind(Close,             "snd_pcm_close") &&
					Bind(ParamsMalloc,      "snd_pcm_hw_params_malloc") &&
					Bind(ParamsFree,        "snd_pcm_hw_params_free") &&
					Bind(ParamsAny,         "snd_pcm_hw_params_any") &&
					Bind(SetAccess,         "snd_pcm_hw_params_set_access") &&
					Bind(SetFormat,         "snd_pcm_hw_params_set_format") &&
					Bind(SetChannelsNear,   "snd_pcm_hw_params_set_channels_near") &&
					Bind(SetRateNear,       "snd_pcm_hw_params_set_rate_near") &&
					Bind(SetPeriodSizeNear, "snd_pcm_hw_params_set_period_size_near") &&
					Bind(SetBufferSizeNear, "snd_pcm_hw_params_set_buffer_size_near") &&
					Bind(ApplyParams,       "snd_pcm_hw_params") &&
					Bind(Prepare,           "snd_pcm_prepare") &&
					Bind(WriteInterleaved,  "snd_pcm_writei") &&
					Bind(Recover,           "snd_pcm_recover") &&
					Bind(Drop,              "snd_pcm_drop") &&
					Bind(StrError,          "snd_strerror");

				if (!bBound)
				{
					Unload();
					return false;
				}

				return true;
			}

			void Unload()
			{
				if (Library != nullptr)
				{
					dlclose(Library);
					Library = nullptr;
				}
			}
		};

		class FLinuxAudioDevice final : public IAudioDevice
		{
		public:

			FLinuxAudioDevice(const FAudioDeviceConfig& InConfig, IAudioRenderCallback* InCallback)
				: Config(InConfig)
				, Callback(InCallback)
			{
			}

			~FLinuxAudioDevice() override
			{
				Stop();
				CloseEndpoint();
				Alsa.Unload();
			}

			bool Start() override;
			void Stop() override;

			bool IsRunning() const override { return bRunning.load(Atomic::MemoryOrderAcquire); }
			bool NeedsRestart() const override { return bNeedsRestart.load(Atomic::MemoryOrderAcquire); }

			uint32 GetSampleRate() const override { return SampleRate; }
			uint32 GetChannelCount() const override { return Channels; }
			uint32 GetPeriodFrames() const override { return PeriodFrames; }

		private:

			bool OpenEndpoint();
			void CloseEndpoint();
			void RenderLoop();

			FAudioDeviceConfig    Config;
			IAudioRenderCallback* Callback = nullptr;

			FAlsaApi Alsa;
			void* Pcm = nullptr;

			TVector<float> Scratch;
			FThread Thread;

			TAtomic<bool> bRunning{false};
			TAtomic<bool> bStopRequested{false};
			TAtomic<bool> bNeedsRestart{false};

			uint32 SampleRate   = 0;
			uint32 Channels     = 0;
			uint32 PeriodFrames = 0;
		};

		bool FLinuxAudioDevice::OpenEndpoint()
		{
			if (!Alsa.Load())
			{
				return false;
			}

			int Result = Alsa.Open(&Pcm, "default", kStreamPlayback, 0);
			if (Result < 0 || Pcm == nullptr)
			{
				LOG_ERROR("[Audio] snd_pcm_open failed ({0})", Alsa.StrError(Result));
				Pcm = nullptr;
				return false;
			}

			void* Params = nullptr;
			if (Alsa.ParamsMalloc(&Params) < 0 || Params == nullptr)
			{
				LOG_ERROR("[Audio] snd_pcm_hw_params_malloc failed");
				return false;
			}

			unsigned int WantedRate     = Config.SampleRate != 0 ? Config.SampleRate : 48000;
			unsigned int WantedChannels = Config.Channels != 0 ? Config.Channels : 2;
			unsigned long WantedPeriod  = Config.PeriodFrames != 0 ? Config.PeriodFrames : kDefaultPeriodFrames;

			bool bConfigured =
				Alsa.ParamsAny(Pcm, Params) >= 0 &&
				Alsa.SetAccess(Pcm, Params, kAccessRwInterleaved) >= 0 &&
				Alsa.SetFormat(Pcm, Params, kFormatFloatLe) >= 0 &&
				Alsa.SetChannelsNear(Pcm, Params, &WantedChannels) >= 0 &&
				Alsa.SetRateNear(Pcm, Params, &WantedRate, nullptr) >= 0 &&
				Alsa.SetPeriodSizeNear(Pcm, Params, &WantedPeriod, nullptr) >= 0;

			if (bConfigured)
			{
				// Several periods of slack, so one late wake-up does not underrun the card.
				unsigned long WantedBuffer = WantedPeriod * kPeriodsPerBuffer;
				Alsa.SetBufferSizeNear(Pcm, Params, &WantedBuffer);

				bConfigured = Alsa.ApplyParams(Pcm, Params) >= 0;
			}

			Alsa.ParamsFree(Params);

			if (!bConfigured)
			{
				LOG_ERROR("[Audio] could not configure the ALSA device for {0} Hz {1}ch", (uint32)WantedRate, (uint32)WantedChannels);
				return false;
			}

			Result = Alsa.Prepare(Pcm);
			if (Result < 0)
			{
				LOG_ERROR("[Audio] snd_pcm_prepare failed ({0})", Alsa.StrError(Result));
				return false;
			}

			SampleRate   = (uint32)WantedRate;
			Channels     = (uint32)WantedChannels;
			PeriodFrames = (uint32)WantedPeriod;

			Scratch.assign((size_t)PeriodFrames * Channels, 0.0f);

			LOG_INFO("[Audio] ALSA at {0} Hz, {1} channels, {2} frame period", SampleRate, Channels, PeriodFrames);
			return true;
		}

		void FLinuxAudioDevice::CloseEndpoint()
		{
			if (Pcm != nullptr)
			{
				Alsa.Drop(Pcm);
				Alsa.Close(Pcm);
				Pcm = nullptr;
			}
		}

		bool FLinuxAudioDevice::Start()
		{
			if (bRunning.load(Atomic::MemoryOrderAcquire) || Callback == nullptr)
			{
				return false;
			}

			if (Pcm == nullptr && !OpenEndpoint())
			{
				CloseEndpoint();
				return false;
			}

			bStopRequested.store(false, Atomic::MemoryOrderRelease);
			bRunning.store(true, Atomic::MemoryOrderRelease);

			Thread = FThread([this]() { RenderLoop(); });
			return true;
		}

		void FLinuxAudioDevice::Stop()
		{
			if (!Thread.Joinable())
			{
				return;
			}

			bStopRequested.store(true, Atomic::MemoryOrderRelease);
			Thread.Join();
			bRunning.store(false, Atomic::MemoryOrderRelease);
		}

		void FLinuxAudioDevice::RenderLoop()
		{
			while (!bStopRequested.load(Atomic::MemoryOrderAcquire))
			{
				Callback->RenderAudio(Scratch.data(), PeriodFrames);

				const float* At = Scratch.data();
				unsigned long Remaining = PeriodFrames;

				while (Remaining > 0 && !bStopRequested.load(Atomic::MemoryOrderAcquire))
				{
					const long Written = Alsa.WriteInterleaved(Pcm, At, Remaining);
					if (Written < 0)
					{
						// Recovers an underrun or a suspend in place; anything else means the endpoint is gone.
						if (Alsa.Recover(Pcm, (int)Written, 1) < 0)
						{
							LOG_ERROR("[Audio] ALSA write failed ({0})", Alsa.StrError((int)Written));
							bNeedsRestart.store(true, Atomic::MemoryOrderRelease);
							return;
						}
						break;
					}

					At        += (size_t)Written * Channels;
					Remaining -= (unsigned long)Written;
				}
			}
		}
	}

	TUniquePtr<IAudioDevice> Audio::CreateDevice(const FAudioDeviceConfig& Config, IAudioRenderCallback* Callback)
	{
		if (Callback == nullptr)
		{
			return nullptr;
		}

		TUniquePtr<FLinuxAudioDevice> Device = MakeUnique<FLinuxAudioDevice>(Config, Callback);
		if (!Device->Start())
		{
			return nullptr;
		}

		return Device;
	}
}

#endif
