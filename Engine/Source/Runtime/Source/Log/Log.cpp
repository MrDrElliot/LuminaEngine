#include "RuntimePCH.h"
#include "Log.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <thread>

#include "LogFormat.h"
#include "LogSink.h"
#include "Sinks/FileSink.h"
#include "Sinks/MemorySink.h"
#include "Sinks/StdoutSink.h"
#include "Containers/Array.h"
#include "Core/Templates/LuminaTemplate.h"
#include "Core/Threading/Thread.h"
#include "Platform/Process/PlatformProcess.h"


// Async pipeline: a call site formats into a per-thread buffer, copies the bytes into a lock-free ring
// slot and returns. One backend thread turns slots into lines, writes a whole drain per sink in one
// call, and flushes to the OS once per batch.

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

		// Vyukov bounded MPMC ring: producers claim a slot with one CAS on EnqueuePos and publish with a
		// release store to the slot's sequence; the single consumer walks positions in order.
		FLogSlot GSlots[GQueueCapacity];

		alignas(CACHE_LINE_SIZE) std::atomic<uint64> GEnqueuePos{ 0 };
		alignas(CACHE_LINE_SIZE) std::atomic<uint64> GDequeuePos{ 0 };

		std::atomic<bool>   GBackendRunning{ false };
		std::atomic<bool>   GStopRequested{ false };
		std::atomic<bool>   GBackendSleeping{ false };
		std::atomic<uint64> GDroppedMessages{ 0 };

		std::thread             GBackendThread;
		std::mutex              GWakeMutex;
		std::condition_variable GWakeCv;

		// Flush handshake: a caller publishes the position it needs drained; the backend bumps the
		// generation once it is past that and every sink has reached the OS.
		std::atomic<uint64>     GFlushTarget{ 0 };
		std::atomic<uint64>     GFlushGeneration{ 0 };
		std::mutex              GFlushMutex;
		std::condition_variable GFlushCv;

		std::mutex                      GSinkMutex;
		TVector<TUniquePtr<ILogSink>>   GSinks;

		// Owned by GSinks; kept for SetLogFileDirectory. Only touched under GSinkMutex.
		FFileSink*                      GFileSink = nullptr;

		constexpr const char* GLogFileName = "Lumina.log";

		// Guards the direct-to-stdout path used before Init() and after Shutdown().
		std::mutex GFallbackMutex;


		FORCEINLINE void WakeBackend()
		{
			if (GBackendSleeping.load(std::memory_order_acquire))
			{
				std::lock_guard Lock(GWakeMutex);
				GWakeCv.notify_one();
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
						Slot.TimeNs   = std::chrono::duration_cast<std::chrono::nanoseconds>(
											std::chrono::system_clock::now().time_since_epoch()).count();
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
				const std::time_t Raw = static_cast<std::time_t>(Seconds);
				std::tm Local{};

			#if defined(_WIN32)
				localtime_s(&Local, &Raw);
			#else
				localtime_r(&Raw, &Local);
			#endif

				const uint32 Year = static_cast<uint32>(Local.tm_year) + 1900;
				WriteTwo(Stamp.Date + 0, Year / 100);
				WriteTwo(Stamp.Date + 2, Year % 100);
				Stamp.Date[4] = '-';
				WriteTwo(Stamp.Date + 5, static_cast<uint32>(Local.tm_mon) + 1);
				Stamp.Date[7] = '-';
				WriteTwo(Stamp.Date + 8, static_cast<uint32>(Local.tm_mday));
				Stamp.Date[10] = '\0';

				WriteTwo(Stamp.Clock + 0, static_cast<uint32>(Local.tm_hour));
				Stamp.Clock[2] = ':';
				WriteTwo(Stamp.Clock + 3, static_cast<uint32>(Local.tm_min));
				Stamp.Clock[5] = ':';
				WriteTwo(Stamp.Clock + 6, static_cast<uint32>(Local.tm_sec));
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

			std::lock_guard Lock(GFallbackMutex);
			std::fwrite("[", 1, 1, stdout);
			std::fwrite(Descriptor.Name.data(), 1, Descriptor.Name.size(), stdout);
			std::fwrite("] ", 1, 2, stdout);
			std::fwrite(Text, 1, Length, stdout);
			std::fwrite("\n", 1, 1, stdout);
			std::fflush(stdout);
		}


		void FlushSinks()
		{
			std::scoped_lock Lock(GSinkMutex);
			for (TUniquePtr<ILogSink>& Sink : GSinks)
			{
				Sink->Flush();
			}
		}


		// Returns how many messages it moved.
		uint32 DrainBatch(FTimestampCache& Timestamps)
		{
			std::scoped_lock Lock(GSinkMutex);

			uint32 Drained = 0;

			// Reported inline so a stall is visible rather than silently swallowing messages.
			if (const uint64 Dropped = GDroppedMessages.exchange(0, std::memory_order_relaxed); Dropped > 0)
			{
				FFixedString Notice;
				std::format_to(std::back_inserter(Notice),
					"Log queue overflowed; {} low-severity message(s) were dropped.", Dropped);

				const int64 Now = std::chrono::duration_cast<std::chrono::nanoseconds>(
									std::chrono::system_clock::now().time_since_epoch()).count();

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
				std::scoped_lock Lock(GFlushMutex);
				GFlushGeneration.fetch_add(1, std::memory_order_release);
			}
			GFlushCv.notify_all();
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
				std::unique_lock Lock(GWakeMutex);
				GBackendSleeping.store(true, std::memory_order_release);

				if (IsQueueEmpty()
					&& !GStopRequested.load(std::memory_order_acquire)
					&& GFlushTarget.load(std::memory_order_acquire) == 0)
				{
					GWakeCv.wait_for(Lock, std::chrono::milliseconds(50));
				}

				GBackendSleeping.store(false, std::memory_order_release);
			}

			Threading::ShutdownThreadHeap();
		}


		// Reused for the process lifetime, so a log call allocates nothing once it reaches high water.
		TFixedString<1024>& GetFormatBuffer()
		{
			thread_local TFixedString<1024> Buffer;
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

		std::scoped_lock Lock(GSinkMutex);
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

		// Keeps prior run evidence; WindowedApp builds have no console. Starts next to the exe
		// because no project is known this early; SetLogFileDirectory moves it once one loads.
		{
			const std::filesystem::path ExePath(Platform::BaseDir());
			const std::filesystem::path LogPath = ExePath.parent_path() / "Logs" / GLogFileName;

			constexpr uint64 MaxLogSizeBytes = 16llu * 1024 * 1024;
			constexpr uint32 MaxLogFiles     = 5;

			TUniquePtr<FFileSink> FileSink = MakeUnique<FFileSink>(
				FString(LogPath.string().c_str()), MaxLogSizeBytes, MaxLogFiles);

			if (FileSink->IsOpen())
			{
				std::scoped_lock Lock(GSinkMutex);
				GFileSink = FileSink.get();
				GSinks.push_back(Move(FileSink));
			}
		}

		// Shipping must keep info: LOG_DISPLAY emits at info, so a higher floor drops boot breadcrumbs.
		#if defined(LUMINA_VERBOSE_LOGGING)
		SetLevel(ELogLevel::Trace);
		#else
		SetLevel(ELogLevel::Info);
		#endif

		GStopRequested.store(false, std::memory_order_relaxed);
		GBackendRunning.store(true, std::memory_order_release);
		GBackendThread = std::thread(&BackendMain);

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
			std::scoped_lock Lock(GWakeMutex);
			GWakeCv.notify_all();
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
			std::scoped_lock Lock(GSinkMutex);
			GFileSink = nullptr;
			GSinks.clear();
		}
	}


	void SetLogFileDirectory(FStringView Directory)
	{
		if (Directory.empty())
		{
			return;
		}

		std::scoped_lock Lock(GSinkMutex);
		if (GFileSink == nullptr)
		{
			return;
		}

		const std::filesystem::path LogPath =
			std::filesystem::path(FString(Directory.data(), Directory.size()).c_str()) / GLogFileName;

		// Retarget flushes what the sink is holding, so nothing written so far is lost.
		GFileSink->Retarget(FString(LogPath.string().c_str()));
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

		std::unique_lock Lock(GFlushMutex);
		GFlushCv.wait_for(Lock, std::chrono::seconds(2), [Generation] 
		{
			return GFlushGeneration.load(std::memory_order_acquire) > Generation; 
		});
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

		// No flush handshake even for Critical: every batch already reaches the OS, and the crash and
		// assert paths call Flush() once after their whole burst.
	}


	void DispatchFormatted(ELogLevel Level, std::string_view Fmt, std::format_args Args) noexcept
	{
		if (!ShouldLog(Level))
		{
			return;
		}

		TFixedString<1024>& Buffer = GetFormatBuffer();
		Buffer.clear();

		try
		{
			std::vformat_to(std::back_inserter(Buffer), Fmt, Args);
		}
		catch (const std::exception& Error)
		{
			Buffer.clear();
			Buffer += "<log format error: ";
			Buffer += Error.what();
			Buffer += ">";
		}

		Dispatch(Level, Buffer.data(), static_cast<uint32>(Buffer.size()));
	}
}
