#pragma once

#include "Memory/SmartPtr.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
	/** Requested output format. A zero field means whatever the endpoint natively runs at. */
	struct FAudioDeviceConfig
	{
		uint32 SampleRate = 0;
		uint32 Channels = 0;
		uint32 PeriodFrames = 0;
	};

	/** Implemented by the mixer. Called on the backend's realtime thread, so it must not lock or allocate. */
	class IAudioRenderCallback
	{
	public:

		virtual ~IAudioRenderCallback() = default;

		virtual void RenderAudio(float* OutInterleaved, uint32 FrameCount) = 0;
	};

	/** One running output stream. The format is fixed for its lifetime, so a change means a new device. */
	class RUNTIME_API IAudioDevice
	{
	public:

		virtual ~IAudioDevice() = default;

		virtual bool Start() = 0;
		virtual void Stop() = 0;
		virtual bool IsRunning() const = 0;

		virtual uint32 GetSampleRate() const = 0;
		virtual uint32 GetChannelCount() const = 0;
		virtual uint32 GetPeriodFrames() const = 0;

		// Set when the endpoint went away underneath us, so the owner rebuilds rather than going silent.
		virtual bool NeedsRestart() const { return false; }
	};

	namespace Audio
	{
		// Null when the platform has no backend yet or the endpoint refused to open.
		RUNTIME_API TUniquePtr<IAudioDevice> CreateDevice(const FAudioDeviceConfig& Config, IAudioRenderCallback* Callback);
	}
}
