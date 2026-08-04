#pragma once

#include <atomic>
#include <format>

#include "Containers/Array.h"
#include "LogLevel.h"
#include "LogMessage.h"


namespace Lumina::Logging
{
	using FLogQueue = TRingBuffer<FConsoleMessage>;

	RUNTIME_API bool IsInitialized();
	RUNTIME_API void Init();
	RUNTIME_API void Shutdown();

	// Blocks until everything logged so far has reached the sinks and the OS.
	RUNTIME_API void Flush();

	// Moves the log file into Directory (created if needed), carrying this run's lines with it.
	// Init() has no project yet, so the file starts beside the exe and lands here once one loads.
	RUNTIME_API void SetLogFileDirectory(FStringView Directory);

	RUNTIME_API void ClearLogQueue();
	RUNTIME_API FLogQueue& GetConsoleLogQueue();

	// Exported as data so the gate below stays inline across module boundaries. Write via SetLevel().
	RUNTIME_API extern std::atomic<uint8> GLevelThreshold;

	RUNTIME_API void SetLevel(ELogLevel Level);

	NODISCARD inline ELogLevel GetLevel() noexcept
	{
		return static_cast<ELogLevel>(GLevelThreshold.load(std::memory_order_relaxed));
	}

	NODISCARD FORCEINLINE bool ShouldLog(ELogLevel Level) noexcept
	{
		return static_cast<uint8>(Level) >= GLevelThreshold.load(std::memory_order_relaxed);
	}

	RUNTIME_API void Dispatch(ELogLevel Level, const char* Text, uint32 Length) noexcept;
	RUNTIME_API void DispatchFormatted(ELogLevel Level, std::string_view Fmt, std::format_args Args) noexcept;

	// Verbatim: no format string is parsed, so braces in the text are harmless.
	FORCEINLINE void Log(ELogLevel Level, FStringView Text) noexcept
	{
		if (ShouldLog(Level))
		{
			Dispatch(Level, Text.data(), static_cast<uint32>(Text.size()));
		}
	}

	// Type-erased so <format> is instantiated once in Log.cpp instead of at every call site.
	template<typename... TArgs> requires (sizeof...(TArgs) > 0)
	FORCEINLINE void Log(ELogLevel Level, std::format_string<TArgs...> Fmt, TArgs&&... Args)
	{
		if (ShouldLog(Level))
		{
			DispatchFormatted(Level, Fmt.get(), std::make_format_args(Args...));
		}
	}

	// Lone non-string argument. Constrained off string-like types so literals keep the verbatim path.
	template<typename T> requires (!std::convertible_to<const T&, FStringView>)
	FORCEINLINE void Log(ELogLevel Level, const T& Value)
	{
		if (ShouldLog(Level))
		{
			DispatchFormatted(Level, "{}", std::make_format_args(Value));
		}
	}
}

// WARN/ERROR/CRITICAL always compile in, they carry crash diagnostics you
// want even in a shipped build.
#define LOG_CRITICAL(...)	::Lumina::Logging::Log(::Lumina::ELogLevel::Critical, __VA_ARGS__)
#define LOG_ERROR(...)		::Lumina::Logging::Log(::Lumina::ELogLevel::Error,    __VA_ARGS__)
#define LOG_WARN(...)		::Lumina::Logging::Log(::Lumina::ELogLevel::Warn,     __VA_ARGS__)

// DISPLAY always compiles in (info severity): rare boot/system milestones that must survive Shipping.
// Not a general info channel, use LOG_INFO for everyday status.
#define LOG_DISPLAY(...)	::Lumina::Logging::Log(::Lumina::ELogLevel::Info,     __VA_ARGS__)

// TRACE/DEBUG/INFO are gated by LUMINA_VERBOSE_LOGGING; off (Shipping default) they expand to nothing.
#if defined(LUMINA_VERBOSE_LOGGING)
	#define LOG_TRACE(...)	::Lumina::Logging::Log(::Lumina::ELogLevel::Trace,    __VA_ARGS__)
	#define LOG_DEBUG(...)	::Lumina::Logging::Log(::Lumina::ELogLevel::Debug,    __VA_ARGS__)
	#define LOG_INFO(...)	::Lumina::Logging::Log(::Lumina::ELogLevel::Info,     __VA_ARGS__)
#else
	#define LOG_TRACE(...)	((void)0)
	#define LOG_DEBUG(...)	((void)0)
	#define LOG_INFO(...)	((void)0)
#endif
