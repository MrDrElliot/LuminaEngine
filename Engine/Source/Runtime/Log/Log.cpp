#include "pch.h"
#include "Log.h"

#include "Core/Console/ConsoleVariable.h"

PRAGMA_DISABLE_ALL_WARNINGS
#include <filesystem>
#include "Sinks/ConsoleSink.h"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include "spdlog/sinks/ringbuffer_sink.h"
PRAGMA_ENABLE_ALL_WARNINGS

#include "Platform/Process/PlatformProcess.h"

// LogMessage.h can't name spdlog's enumerators (it only forward-declares the enum to keep spdlog
// out of every TU that reaches Log.h), so verify its hardcoded default here where spdlog is complete.
static_assert(Lumina::GDefaultConsoleMessageLevel == static_cast<int>(spdlog::level::info),
              "GDefaultConsoleMessageLevel is out of sync with spdlog::level::info");

namespace Lumina::Logging
{

	static std::shared_ptr<spdlog::logger> Logger;
	
	
	bool IsInitialized()
	{
		return Logger != nullptr;
	}

	void Init()
	{
		spdlog::set_pattern("%^[%T] %n: %v%$");
		Logger = spdlog::stdout_color_mt("Lumina");
		Logger->sinks().push_back(std::make_shared<FConsoleSink>(GetConsoleLogQueue()));

		// Rotating file sink: keeps prior run evidence; WindowedApp builds have no console.
		try
		{
			std::filesystem::path ExePath(Platform::BaseDir());
			std::filesystem::path LogPath = ExePath.parent_path() / "Lumina.log";

			constexpr size_t MaxLogSizeBytes = 16 * 1024 * 1024;
			constexpr size_t MaxLogFiles     = 5;
			auto FileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
				LogPath.string(), MaxLogSizeBytes, MaxLogFiles, /*rotate_on_open=*/true);
			FileSink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
			Logger->sinks().push_back(FileSink);
		}
		catch (const std::exception&)
		{
			// Non-fatal; fall back to console-only logging.
		}

		// Shipping must keep info level: LOG_DISPLAY emits at info, so a higher filter would drop boot breadcrumbs.
		#if defined(LUMINA_VERBOSE_LOGGING)
		Logger->set_level(spdlog::level::trace);
		#else
		Logger->set_level(spdlog::level::info);
		#endif
		// Flush eagerly (at info) so boot breadcrumbs reach disk before a crash/hang.
		Logger->flush_on(spdlog::level::info);

		LOG_TRACE("------- Log Initialized -------");
	}

	void Shutdown()
	{
		LOG_TRACE("------- Log Shutdown -------");
		spdlog::shutdown();
		Logger = nullptr;
	}

	void ClearLogQueue()
	{
		GetConsoleLogQueue().clear();
	}

	FLogQueue& GetConsoleLogQueue()
	{
		static FLogQueue Logs(300);
		return Logs;
	}

	const std::shared_ptr<spdlog::logger>& GetLogger()
	{
		return Logger;
	}
	
	const std::shared_ptr<spdlog::sinks::sink>& GetSink()
	{
		return Logger->sinks().front();
	}
	
}
