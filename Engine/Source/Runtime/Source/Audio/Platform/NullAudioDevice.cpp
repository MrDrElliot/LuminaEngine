#include "RuntimePCH.h"
#if !defined(_WIN32) && !defined(LE_PLATFORM_LINUX)

#include "Audio/AudioDevice.h"
#include "Log/Log.h"

namespace Lumina
{
	TUniquePtr<IAudioDevice> Audio::CreateDevice(const FAudioDeviceConfig& Config, IAudioRenderCallback* Callback)
	{
		(void)Config;
		(void)Callback;

		LOG_WARN_ONCE("[Audio] no output backend is implemented for this platform yet");
		return nullptr;
	}
}

#endif
