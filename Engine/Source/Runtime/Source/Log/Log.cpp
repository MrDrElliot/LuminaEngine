#include "Platform/Time/PlatformTime.h"
#include "RuntimePCH.h"
#include <iterator>
#include <string_view>
#include "Log.h"

#include <cstdio>
#include <ctime>

#include "LogFormat.h"
#include "Containers/StringFormat.h"
#include "LogSink.h"
#include "Sinks/FileSink.h"
#include "Sinks/MemorySink.h"
#include "Sinks/StdoutSink.h"
#include "Containers/Vector.h"
#include "Core/Templates/LuminaTemplate.h"
#include "Core/Threading/Thread.h"
#include "Platform/Process/PlatformProcess.h"


// A call site formats, copies into a lock-free ring slot and returns, and one backend thread drains.

namespace Lumina::Logging
{
	RUNTIME_API std::atomic<uint8> GLevelThreshold{ static_cast<uint8>(ELogLevel::Info) };

	namespace
	{
		constexpr uint32 GQueueCapacity     = 4096;         // power of two
		constexpr uint32 GQueueMask         = GQueueCapacity - 1;
		constexpr uint32 GInlineTextBytes   = 216;          // keeps a slot at one 256B stride
		constexpr uint32 GMaxMessageBytes   = 16 * 1024;    // anything longer is truncated
		constexpr uint32 GMaxDrainPerBatch  = 1024;
		constexpr uint32 kDefaultConsoleLogQueueCapacity = 300;
		constexpr uint32 GEnqueueSpinLimit  = 256;

		struct alignas(64) FLogSlot
		{
			std::atomic<uint64> Sequence;
			int64               TimeNs;
			char*               Heap;       // owned, non-null only when Length > GInlineTextBytes
			uint32              ThreadId;
			uint32              Length;
			ELogLevel           Level;
			char                Inline[GInlineTextBytes];
		};

		static_assert(sizeof(FLogSlot) == 256, "FLogSlot should occupy exactly four cache lines");

		// A Vyukov bounded MPMC ring, so producers claim with one CAS and the single consumer walks in order.
		FLogSlot GSlots[GQueueCapacity];

		alignas(CACHE_LINE_SIZE) std::atomic<uint64> GEnqueuePos{ 0 };
		alignas(CACHE_LINE_SIZE) std::atomic<uint64> GDequeuePos{ 0 };

		std::atomic<bool>   GBackendRunning{ false };
		std::atomic<bool>   GStopRequested{ false };
		std::atomic<bool>   GBackendSleeping{ false };
		std::atomic<uint64> GDroppedMessages{ 0 };

		FThread             GBackendThread;
		FMutex              GWakeMutex;
		FConditionVariable GWakeCv;

		// A caller publishes the position it needs drained, and the backend bumps the generation past it.
		std::atomic<uint64>     GFlushTarget{ 0 };
		std::atomic<uint64>     GFlushGeneration{ 0 };
		FMutex              GFlushMutex;
		FConditionVariable GFlushCv;

		FMutex                      GSinkMutex;
		TVector<TUniquePtr<ILogSink>>   GSinks;

		// Owned by GSinks; kept for SetLogFileDirectory. Only touched under GSinkMutex.
		FFileSink*                      GFileSink = nullptr;

		// Not constant, since a standalone launch renames it to keep off the editor's file.
		FString GLogFileName = "Lumina.log";

		// Guards the direct-to-stdout path used before Init() and after Shutdown().
		FMutex GFallbackMutex;


		FORCEINLINE void WakeBackend()
		{
			if (GBackendSleeping.load(std::memory_order_acquire))
			{
				FScopeLock Lock(GWakeMutex);
				GWakeCv.NotifyOne();
			}
		}


		bool TryEnqueue(ELogLevel Level, const char* Text, uint32 Length)
		{
			uint64 Pos = GEnqueuePos.load(std::memory_order_relaxed);

			for (;;)
			{
				FLogSlot& Slot = GSlots[Pos & GQueueMask];
				const uint64 Sequence = Slot.Sequence.load(std::memory_order_acquire);
				const int64 Diff = static_cast<int64>(Sequence) - static_cast<int64>(Pos);

				if (Diff == 0)
				{
					if (GEnqueuePos.compare_exchange_weak(Pos, Pos + 1, std::memory_order_relaxed))
					{
						Slot.TimeNs   = PlatformTime::UtcNanoseconds();
						Slot.ThreadId = static_cast<uint32>(Threading::GetThreadID());
						Slot.Level    = Level;
						Slot.Length   = Length;

						if (Length <= GInlineTextBytes)
						{
							Slot.Heap = nullptr;
							std::memcpy(Slot.Inline, Text, Length);
						}
						else
						{
							Slot.Heap = static_cast<char*>(Memory::Malloc(Length));
							std::memcpy(Slot.Heap, Text, Length);
						}

						Slot.Sequence.store(Pos + 1, std::memory_order_release);
						return true;
					}
				}
				else if (Diff < 0)
				{
					return false;   // slot still occupied
				}
				else
				{
					Pos = GEnqueuePos.load(std::memory_order_relaxed);
				}
			}
		}


		// Single consumer, so the position needs no read-modify-write.
		FLogSlot* TryDequeue()
		{
			const uint64 Pos = GDequeuePos.load(std::memory_order_relaxed);
			FLogSlot& Slot = GSlots[Pos & GQueueMask];

			if (Slot.Sequence.load(std::memory_order_acquire) != Pos + 1)
			{
				return nullptr;
			}

			GDequeuePos.store(Pos + 1, std::memory_order_relaxed);
			return &Slot;
		}


		void ReleaseSlot(FLogSlot& Slot, uint64 ConsumedPos)
		{
			if (Slot.Heap != nullptr)
			{
				void* Ptr = Slot.Heap;
				Memory::Free(Ptr);
				Slot.Heap = nullptr;
			}

			Slot.Sequence.store(ConsumedPos + GQueueCapacity, std::memory_order_release);
		}


		// Recomputed once a second; a burst inside the same second only re-derives the milliseconds.
		class FTimestampCache
		{
		public:

			const FLogTimestamp& Get(int64 TimeNs)
			{
				constexpr int64 NanosPerSecond = 1'000'000'000;

				const int64 Seconds = TimeNs / NanosPerSecond;
				if (Seconds != CachedSecond)
				{
					CachedSecond = Seconds;
					Rebuild(Seconds);
				}

				Stamp.Millis = static_cast<uint16>((TimeNs % NanosPerSecond) / 1'000'000);
				return Stamp;
			}

		private:

			void Rebuild(int64 Seconds)
			{
				const PlatformTime::FDateTime Local = PlatformTime::LocalTime(Seconds * 1000000000ll);

				const uint32 Year = static_cast<uint32>(Local.Year);
				WriteTwo(Stamp.Date + 0, Year / 100);
				WriteTwo(Stamp.Date + 2, Year % 100);
				Stamp.Date[4] = '-';
				WriteTwo(Stamp.Date + 5, static_cast<uint32>(Local.Month));
				Stamp.Date[7] = '-';
				WriteTwo(Stamp.Date + 8, static_cast<uint32>(Local.Day));
				Stamp.Date[10] = '\0';

				WriteTwo(Stamp.Clock + 0, static_cast<uint32>(Local.Hour));
				Stamp.Clock[2] = ':';
				WriteTwo(Stamp.Clock + 3, static_cast<uint32>(Local.Minute));
				Stamp.Clock[5] = ':';
				WriteTwo(Stamp.Clock + 6, static_cast<uint32>(Local.Second));
				Stamp.Clock[8] = '\0';
			}

			static void WriteTwo(char* Out, uint32 Value)
			{
				std::memcpy(Out, GTwoDigits.Data + (Value % 100) * 2, 2);
			}

			int64         CachedSecond = -1;
			FLogTimestamp Stamp{};
		};


		void WriteFallback(ELogLevel Level, const char* Text, uint32 Length)
		{
			const FLevelDescriptor& Descriptor = GetLevelDescriptor(Level);

			FScopeLock Lock(GFallbackMutex);
			std::fwrite("[", 1, 1, stdout);
			std::fwrite(Descriptor.Name.data(), 1, Descriptor.Name.size(), stdout);
			std::fwrite("] ", 1, 2, stdout);
			std::fwrite(Text, 1, Length, stdout);
			std::fwrite("\n", 1, 1, stdout);
			std::fflush(stdout);
		}


		void FlushSinks()
		{
			FScopeLock Lock(GSinkMutex);
			for (TUniquePtr<ILogSink>& Sink : GSinks)
			{
				Sink->Flush();
			}
		}


		// Returns how many messages it moved.
		uint32 DrainBatch(FTimestampCache& Timestamps)
		{
			FScopeLock Lock(GSinkMutex);

			uint32 Drained = 0;

			// Reported inline so a stall is visible rather than silently swallowing messages.
			if (const uint64 Dropped = GDroppedMessages.exchange(0, std::memory_order_relaxed); Dropped > 0)
			{
				const FFixedString Notice = FormatAs<FFixedString>(
					"Log queue overflowed; {} low-severity message(s) were dropped.", Dropped);

				const int64 Now = PlatformTime::UtcNanoseconds();

				const FLogRecord Record
				{
					FStringView(Notice.data(), Notice.size()),
					&Timestamps.Get(Now),
					ELogLevel::Warn,
					static_cast<uint32>(Threading::GetThreadID()),
				};

				for (TUniquePtr<ILogSink>& Sink : GSinks)
				{
					Sink->Write(Record);
				}
			}

			while (Drained < GMaxDrainPerBatch)
			{
				const uint64 Pos = GDequeuePos.load(std::memory_order_relaxed);
				FLogSlot* Slot = TryDequeue();
				if (Slot == nullptr)
				{
					break;
				}

				const char* Text = Slot->Heap != nullptr ? Slot->Heap : Slot->Inline;

				const FLogRecord Record
				{
					FStringView(Text, Slot->Length),
					&Timestamps.Get(Slot->TimeNs),
					Slot->Level,
					Slot->ThreadId,
				};

				for (TUniquePtr<ILogSink>& Sink : GSinks)
				{
					Sink->Write(Record);
				}

				ReleaseSlot(*Slot, Pos);
				++Drained;
			}

			if (Drained > 0)
			{
				for (TUniquePtr<ILogSink>& Sink : GSinks)
				{
					Sink->Flush();
				}
			}

			return Drained;
		}


		void ServiceFlushRequest()
		{
			const uint64 Target = GFlushTarget.load(std::memory_order_acquire);
			if (Target == 0 || GDequeuePos.load(std::memory_order_relaxed) < Target)
			{
				return;
			}

			FlushSinks();
			GFlushTarget.store(0, std::memory_order_release);

			{
				FScopeLock Lock(GFlushMutex);
				GFlushGeneration.fetch_add(1, std::memory_order_release);
			}
			GFlushCv.NotifyAll();
		}


		bool IsQueueEmpty()
		{
			return GDequeuePos.load(std::memory_order_relaxed) == GEnqueuePos.load(std::memory_order_acquire);
		}


		void BackendMain()
		{
			Threading::SetThreadName("Log");
			Threading::InitializeThreadHeap();

			FTimestampCache Timestamps;

			for (;;)
			{
				const uint32 Drained = DrainBatch(Timestamps);
				ServiceFlushRequest();

				if (Drained > 0)
				{
					continue;
				}

				if (GStopRequested.load(std::memory_order_acquire) && IsQueueEmpty())
				{
					break;
				}

				// Timeout is a backstop; WakeBackend covers the common case.
				FUniqueLock Lock(GWakeMutex);
				GBackendSleeping.store(true, std::memory_order_release);

				if (IsQueueEmpty()
					&& !GStopRequested.load(std::memory_order_acquire)
					&& GFlushTarget.load(std::memory_order_acquire) == 0)
				{
					(void)GWakeCv.WaitFor(Lock, 0.05);
				}

				GBackendSleeping.store(false, std::memory_order_release);
			}

			Threading::ShutdownThreadHeap();
		}


		// Reused for the process lifetime, so a log call allocates nothing once it reaches high water.
		Fmt::TInlineFormatBuffer<1024>& GetFormatBuffer()
		{
			thread_local Fmt::TInlineFormatBuffer<1024> Buffer;
			return Buffer;
		}
	}


	bool IsInitialized()
	{
		return GBackendRunning.load(std::memory_order_acquire);
	}


	void SetLevel(ELogLevel Level)
	{
		GLevelThreshold.store(static_cast<uint8>(Level), std::memory_order_relaxed);
	}


	void AddSink(TUniquePtr<ILogSink> Sink)
	{
		if (Sink == nullptr)
		{
			return;
		}

		FScopeLock Lock(GSinkMutex);
		GSinks.push_back(Move(Sink));
	}


	void Init()
	{
		if (GBackendRunning.load(std::memory_order_acquire))
		{
			return;
		}

		for (uint64 Index = 0; Index < GQueueCapacity; ++Index)
		{
			GSlots[Index].Sequence.store(Index, std::memory_order_relaxed);
			GSlots[Index].Heap = nullptr;
		}
		GEnqueuePos.store(0, std::memory_order_relaxed);
		GDequeuePos.store(0, std::memory_order_relaxed);

		AddSink(MakeUnique<FStdoutSink>());
		AddSink(MakeUnique<FMemorySink>(GetConsoleLogQueue()));

		// Starts beside the exe because no project is known this early, and moves once one loads.
		{
			FString LogPath(TCHAR_TO_UTF8(Platform::BaseDir()));
			const size_t ExeNameStart = LogPath.find_last_of("/\\");
			LogPath.resize(ExeNameStart == FString::npos ? 0 : ExeNameStart);
			LogPath.append("/Logs/");
			LogPath.append(GLogFileName);

			constexpr uint64 MaxLogSizeBytes = 16llu * 1024 * 1024;
			constexpr uint32 MaxLogFiles     = 5;

			TUniquePtr<FFileSink> FileSink = MakeUnique<FFileSink>(
				LogPath, MaxLogSizeBytes, MaxLogFiles);

			if (FileSink->IsOpen())
			{
				FScopeLock Lock(GSinkMutex);
				GFileSink = FileSink.get();
				GSinks.push_back(Move(FileSink));
			}
		}

		// Shipping must keep info, since LOG_DISPLAY emits there and a higher floor drops boot breadcrumbs.
		#if defined(LUMINA_VERBOSE_LOGGING)
		SetLevel(ELogLevel::Trace);
		#else
		SetLevel(ELogLevel::Info);
		#endif

		GStopRequested.store(false, std::memory_order_relaxed);
		GBackendRunning.store(true, std::memory_order_release);
		GBackendThread = FThread(&BackendMain);

		LOG_TRACE("------- Log Initialized -------");
	}


	void Shutdown()
	{
		if (!GBackendRunning.load(std::memory_order_acquire))
		{
			return;
		}

		LOG_TRACE("------- Log Shutdown -------");

		GStopRequested.store(true, std::memory_order_release);
		{
			FScopeLock Lock(GWakeMutex);
			GWakeCv.NotifyAll();
		}

		if (GBackendThread.joinable())
		{
			GBackendThread.join();
		}

		GBackendRunning.store(false, std::memory_order_release);

		// Anything a producer squeezed in between the final drain and the join.
		FTimestampCache Timestamps;
		while (DrainBatch(Timestamps) > 0)
		{
		}
		FlushSinks();

		{
			FScopeLock Lock(GSinkMutex);
			GFileSink = nullptr;
			GSinks.clear();
		}
	}


	FString GetLogFilePath()
	{
		FScopeLock Lock(GSinkMutex);
		return GFileSink != nullptr ? GFileSink->GetBasePath() : FString();
	}


	void SetLogFileDirectory(FStringView Directory)
	{
		if (Directory.empty())
		{
			return;
		}

		FScopeLock Lock(GSinkMutex);
		if (GFileSink == nullptr)
		{
			return;
		}

		FString LogPath(Directory.data(), Directory.size());
		if (!LogPath.empty() && LogPath.back() != '/' && LogPath.back() != '\\')
		{
			LogPath.push_back('/');
		}
		LogPath.append(GLogFileName);

		// Retarget flushes what the sink is holding, so nothing written so far is lost.
		GFileSink->Retarget(LogPath);
	}


	void SetLogFileName(FStringView FileName)
	{
		if (FileName.empty())
		{
			return;
		}

		FScopeLock Lock(GSinkMutex);
		if (GFileSink == nullptr)
		{
			return;
		}

		GLogFileName.assign(FileName.data(), FileName.size());
		if (!GLogFileName.ends_with(".log"))
		{
			GLogFileName.append(".log");
		}

		FString NewPath = GFileSink->GetBasePath();
		const size_t LastSlash = NewPath.find_last_of("/\\");
		NewPath.resize(LastSlash == FString::npos ? 0 : LastSlash + 1);
		NewPath.append(GLogFileName);

		GFileSink->Retarget(NewPath);
	}


	void Flush()
	{
		if (!GBackendRunning.load(std::memory_order_acquire))
		{
			FlushSinks();
			return;
		}

		const uint64 Target = GEnqueuePos.load(std::memory_order_acquire);
		if (Target == 0)
		{
			return;
		}

		const uint64 Generation = GFlushGeneration.load(std::memory_order_acquire);

		// Keep the highest outstanding target so concurrent flushes don't shorten each other.
		uint64 Current = GFlushTarget.load(std::memory_order_relaxed);
		while (Current < Target
			&& !GFlushTarget.compare_exchange_weak(Current, Target, std::memory_order_release, std::memory_order_relaxed))
		{
		}

		WakeBackend();

		FUniqueLock Lock(GFlushMutex);
		GFlushCv.WaitFor(Lock, 2.0, [Generation]
		{
			return GFlushGeneration.load(std::memory_order_acquire) > Generation;
		});
	}


	// The backend thread pushes into this queue under GSinkMutex, so any resize or clear takes it too.
	void ClearLogQueue()
	{
		FScopeLock Lock(GSinkMutex);
		GetConsoleLogQueue().clear();
	}


	FLogQueue& GetConsoleLogQueue()
	{
		static FLogQueue Logs(kDefaultConsoleLogQueueCapacity);
		return Logs;
	}


	void SetConsoleLogQueueCapacity(uint32 Capacity)
	{
		FScopeLock Lock(GSinkMutex);
		GetConsoleLogQueue().set_capacity(Capacity == 0 ? 1u : Capacity);
	}


	uint32 GetConsoleLogQueueCapacity()
	{
		FScopeLock Lock(GSinkMutex);
		return (uint32)GetConsoleLogQueue().capacity();
	}


	void Dispatch(ELogLevel Level, const char* Text, uint32 Length) noexcept
	{
		if (!ShouldLog(Level))
		{
			return;
		}

		Length = Length > GMaxMessageBytes ? GMaxMessageBytes : Length;

		if (!GBackendRunning.load(std::memory_order_acquire))
		{
			WriteFallback(Level, Text, Length);
			return;
		}

		// Errors and worse are never dropped; anything below gives up rather than stalling the frame.
		const bool bMustLand = Level >= ELogLevel::Error;

		for (uint32 Spins = 0; !TryEnqueue(Level, Text, Length); ++Spins)
		{
			WakeBackend();

			if (!bMustLand && Spins >= GEnqueueSpinLimit)
			{
				GDroppedMessages.fetch_add(1, std::memory_order_relaxed);
				return;
			}

			Threading::ThreadYield();
		}

		WakeBackend();
	}


	void DispatchFormatted(ELogLevel Level, FStringView Format, Fmt::FFormatArgs Args) noexcept
	{
		if (!ShouldLog(Level))
		{
			return;
		}

		Fmt::TInlineFormatBuffer<1024>& Buffer = GetFormatBuffer();
		Buffer.Clear();
		Fmt::VFormatTo(Buffer, Format, Args);

		Dispatch(Level, Buffer.Data(), static_cast<uint32>(Buffer.Size()));
	}
}
