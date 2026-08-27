#include "RuntimePCH.h"
#ifdef _WIN32

#include "Audio/AudioDevice.h"
#include "Core/Math/Scalar.h"
#include "Core/Threading/Atomic.h"
#include "Core/Threading/Thread.h"
#include "Log/Log.h"

#pragma warning(push)
#pragma warning(disable: 4201)
#include <Windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#pragma warning(pop)

#pragma comment(lib, "ole32.lib")

namespace Lumina
{
	namespace
	{
		// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, spelled out so this does not need ksmedia.h.
		const GUID kSubtypeIeeeFloat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

		constexpr uint32 kDefaultPeriodFrames = 480;
		constexpr uint32 kReferenceTimesPerSecond = 10000000;

		template <typename T>
		void SafeRelease(T*& Pointer)
		{
			if (Pointer != nullptr)
			{
				Pointer->Release();
				Pointer = nullptr;
			}
		}

		class FWindowsAudioDevice final : public IAudioDevice
		{
		public:

			FWindowsAudioDevice(const FAudioDeviceConfig& InConfig, IAudioRenderCallback* InCallback)
				: Config(InConfig)
				, Callback(InCallback)
			{
			}

			~FWindowsAudioDevice() override
			{
				Stop();
			}

			bool Start() override;
			void Stop() override;

			bool IsRunning() const override { return bRunning.load(Atomic::MemoryOrderAcquire); }
			bool NeedsRestart() const override { return bNeedsRestart.load(Atomic::MemoryOrderAcquire); }

			uint32 GetSampleRate() const override { return SampleRate; }
			uint32 GetChannelCount() const override { return Channels; }
			uint32 GetPeriodFrames() const override { return PeriodFrames; }

		private:

			void ThreadMain();
			bool OpenEndpoint();
			void CloseEndpoint();
			void RenderLoop();

			FAudioDeviceConfig    Config;
			IAudioRenderCallback* Callback = nullptr;

			IMMDeviceEnumerator* Enumerator   = nullptr;
			IMMDevice*           Endpoint     = nullptr;
			IAudioClient*        Client       = nullptr;
			IAudioRenderClient*  RenderClient = nullptr;

			HANDLE BufferEvent = nullptr;
			HANDLE ReadyEvent  = nullptr;

			FThread Thread;

			TAtomic<bool> bRunning{false};
			TAtomic<bool> bStopRequested{false};
			TAtomic<bool> bNeedsRestart{false};

			bool bOpened = false;

			uint32 SampleRate   = 0;
			uint32 Channels     = 0;
			uint32 PeriodFrames = 0;
			uint32 BufferFrames = 0;
		};

		bool FWindowsAudioDevice::Start()
		{
			if (bRunning.load(Atomic::MemoryOrderAcquire) || Callback == nullptr)
			{
				return false;
			}

			ReadyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			if (ReadyEvent == nullptr)
			{
				LOG_ERROR("[Audio] failed to create the device startup event");
				return false;
			}

			bStopRequested.store(false, Atomic::MemoryOrderRelease);
			Thread = FThread([this]() { ThreadMain(); });

			// The endpoint is opened on the render thread so every COM call shares one apartment.
			WaitForSingleObject(ReadyEvent, INFINITE);
			CloseHandle(ReadyEvent);
			ReadyEvent = nullptr;

			if (!bRunning.load(Atomic::MemoryOrderAcquire))
			{
				Thread.Join();
				return false;
			}

			return true;
		}

		void FWindowsAudioDevice::Stop()
		{
			if (!Thread.Joinable())
			{
				return;
			}

			bStopRequested.store(true, Atomic::MemoryOrderRelease);

			if (BufferEvent != nullptr)
			{
				SetEvent(BufferEvent);
			}

			Thread.Join();
			bRunning.store(false, Atomic::MemoryOrderRelease);
		}

		void FWindowsAudioDevice::ThreadMain()
		{
			const HRESULT ComResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			const bool bOwnsCom = SUCCEEDED(ComResult);

			if (OpenEndpoint())
			{
				bRunning.store(true, Atomic::MemoryOrderRelease);
			}

			SetEvent(ReadyEvent);

			if (bRunning.load(Atomic::MemoryOrderAcquire))
			{
				SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
				RenderLoop();
			}

			CloseEndpoint();

			if (bOwnsCom)
			{
				CoUninitialize();
			}
		}

		bool FWindowsAudioDevice::OpenEndpoint()
		{
			HRESULT Result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
				__uuidof(IMMDeviceEnumerator), (void**)&Enumerator);
			if (FAILED(Result))
			{
				LOG_ERROR("[Audio] CoCreateInstance(MMDeviceEnumerator) failed (0x{0:X})", (uint32)Result);
				return false;
			}

			Result = Enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &Endpoint);
			if (FAILED(Result))
			{
				LOG_ERROR("[Audio] no default render endpoint (0x{0:X})", (uint32)Result);
				return false;
			}

			Result = Endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&Client);
			if (FAILED(Result))
			{
				LOG_ERROR("[Audio] IAudioClient activation failed (0x{0:X})", (uint32)Result);
				return false;
			}

			WAVEFORMATEX* MixFormat = nullptr;
			Result = Client->GetMixFormat(&MixFormat);
			if (FAILED(Result) || MixFormat == nullptr)
			{
				LOG_ERROR("[Audio] GetMixFormat failed (0x{0:X})", (uint32)Result);
				return false;
			}

			const uint32 WantedRate     = Config.SampleRate != 0 ? Config.SampleRate : (uint32)MixFormat->nSamplesPerSec;
			const uint32 WantedChannels = Config.Channels != 0 ? Config.Channels : (uint32)MixFormat->nChannels;
			CoTaskMemFree(MixFormat);

			WAVEFORMATEXTENSIBLE Desired = {};
			Desired.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
			Desired.Format.nChannels       = (WORD)WantedChannels;
			Desired.Format.nSamplesPerSec  = (DWORD)WantedRate;
			Desired.Format.wBitsPerSample  = 32;
			Desired.Format.nBlockAlign     = (WORD)(WantedChannels * sizeof(float));
			Desired.Format.nAvgBytesPerSec = (DWORD)(WantedRate * Desired.Format.nBlockAlign);
			Desired.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
			Desired.Samples.wValidBitsPerSample = 32;
			Desired.dwChannelMask          = 0;
			Desired.SubFormat              = kSubtypeIeeeFloat;

			const uint32 RequestedPeriod = Config.PeriodFrames != 0 ? Config.PeriodFrames : kDefaultPeriodFrames;
			const REFERENCE_TIME Duration = (REFERENCE_TIME)((uint64)RequestedPeriod * kReferenceTimesPerSecond / WantedRate);

			// AUTOCONVERTPCM lets the endpoint resample, so a project rate that differs from the mix format still opens.
			const DWORD Flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK
				| AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
				| AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

			Result = Client->Initialize(AUDCLNT_SHAREMODE_SHARED, Flags, Duration, 0, &Desired.Format, nullptr);
			if (FAILED(Result))
			{
				LOG_ERROR("[Audio] IAudioClient::Initialize failed for {0} Hz {1}ch (0x{2:X})",
					WantedRate, WantedChannels, (uint32)Result);
				return false;
			}

			UINT32 EndpointFrames = 0;
			Result = Client->GetBufferSize(&EndpointFrames);
			if (FAILED(Result))
			{
				LOG_ERROR("[Audio] GetBufferSize failed (0x{0:X})", (uint32)Result);
				return false;
			}

			BufferEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (BufferEvent == nullptr)
			{
				LOG_ERROR("[Audio] failed to create the render event");
				return false;
			}

			Result = Client->SetEventHandle(BufferEvent);
			if (FAILED(Result))
			{
				LOG_ERROR("[Audio] SetEventHandle failed (0x{0:X})", (uint32)Result);
				return false;
			}

			Result = Client->GetService(__uuidof(IAudioRenderClient), (void**)&RenderClient);
			if (FAILED(Result))
			{
				LOG_ERROR("[Audio] IAudioRenderClient service failed (0x{0:X})", (uint32)Result);
				return false;
			}

			SampleRate   = WantedRate;
			Channels     = WantedChannels;
			BufferFrames = (uint32)EndpointFrames;
			PeriodFrames = Math::Min(RequestedPeriod, BufferFrames);

			Result = Client->Start();
			if (FAILED(Result))
			{
				LOG_ERROR("[Audio] IAudioClient::Start failed (0x{0:X})", (uint32)Result);
				return false;
			}

			bOpened = true;
			LOG_INFO("[Audio] WASAPI shared mode at {0} Hz, {1} channels, {2} frame buffer",
				SampleRate, Channels, BufferFrames);
			return true;
		}

		void FWindowsAudioDevice::CloseEndpoint()
		{
			if (Client != nullptr && bOpened)
			{
				Client->Stop();
				bOpened = false;
			}

			SafeRelease(RenderClient);
			SafeRelease(Client);
			SafeRelease(Endpoint);
			SafeRelease(Enumerator);

			if (BufferEvent != nullptr)
			{
				CloseHandle(BufferEvent);
				BufferEvent = nullptr;
			}
		}

		void FWindowsAudioDevice::RenderLoop()
		{
			while (!bStopRequested.load(Atomic::MemoryOrderAcquire))
			{
				if (WaitForSingleObject(BufferEvent, 2000) != WAIT_OBJECT_0)
				{
					continue;
				}

				if (bStopRequested.load(Atomic::MemoryOrderAcquire))
				{
					break;
				}

				UINT32 Padding = 0;
				HRESULT Result = Client->GetCurrentPadding(&Padding);
				if (FAILED(Result))
				{
					if (Result == AUDCLNT_E_DEVICE_INVALIDATED)
					{
						bNeedsRestart.store(true, Atomic::MemoryOrderRelease);
					}
					break;
				}

				const UINT32 Available = BufferFrames - Padding;
				if (Available == 0)
				{
					continue;
				}

				BYTE* Buffer = nullptr;
				Result = RenderClient->GetBuffer(Available, &Buffer);
				if (FAILED(Result))
				{
					if (Result == AUDCLNT_E_DEVICE_INVALIDATED)
					{
						bNeedsRestart.store(true, Atomic::MemoryOrderRelease);
					}
					break;
				}

				Callback->RenderAudio((float*)Buffer, (uint32)Available);
				RenderClient->ReleaseBuffer(Available, 0);
			}
		}
	}

	TUniquePtr<IAudioDevice> Audio::CreateDevice(const FAudioDeviceConfig& Config, IAudioRenderCallback* Callback)
	{
		if (Callback == nullptr)
		{
			return nullptr;
		}

		TUniquePtr<FWindowsAudioDevice> Device = MakeUnique<FWindowsAudioDevice>(Config, Callback);
		if (!Device->Start())
		{
			return nullptr;
		}

		return Device;
	}
}

#endif
