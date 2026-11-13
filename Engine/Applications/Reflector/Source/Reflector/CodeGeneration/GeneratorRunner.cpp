#include "GeneratorRunner.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <unordered_set>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

#include "CppReflectionGenerator.h"
#include "CSharpBindingGenerator.h"
#include "Reflector/Diagnostics/LRTDiagnostics.h"
#include "Reflector/ReflectionSpecifiers.h"
#include "Reflector/Utils/FileIO.h"

namespace Lumina::Reflection
{
    namespace
    {
        #if defined(_WIN32)
            constexpr DWORD kCppExceptionCode = 0xE06D7363;

            // Holds no object with a destructor, which a __try frame cannot.
            bool CallCatchingFaults(void (*Thunk)(void*), void* Body)
            {
                __try
                {
                    Thunk(Body);
                    return true;
                }
                __except (GetExceptionCode() == kCppExceptionCode ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER)
                {
                    return false;
                }
            }
        #endif

        // Turns a hardware fault in Body into a thrown exception on Windows, so a crashing generator fails like a throwing one.
        template<typename TBody>
        void RunCatchingFaults(TBody& Body)
        {
            #if defined(_WIN32)
                if (!CallCatchingFaults([](void* Erased) { (*static_cast<TBody*>(Erased))(); }, &Body))
                {
                    throw std::runtime_error("it crashed with a hardware fault such as an access violation");
                }
            #else
                Body();
            #endif
        }

        // Empty when Body returned normally, otherwise why it did not.
        template<typename TBody>
        std::string RunIsolated(TBody&& Body)
        {
            try
            {
                RunCatchingFaults(Body);
                return {};
            }
            catch (const std::exception& Error)
            {
                return Error.what();
            }
            catch (...)
            {
                return "it threw an exception that is not a std::exception";
            }
        }

        // Ends the process when a loaded generator overruns its time, which leaves the already written built-in outputs whole.
        class FGeneratorWatchdog
        {
        public:

            explicit FGeneratorWatchdog(std::chrono::seconds InTimeout) : Timeout(InTimeout), Thread([this] { Watch(); }) {}

            ~FGeneratorWatchdog()
            {
                {
                    const std::lock_guard Lock(Mutex);
                    bStopping = true;
                }
                Signal.notify_all();
                Thread.join();
            }

            void Begin(const IReflectionGenerator& Generator, const std::string& RulesFile)
            {
                const std::lock_guard Lock(Mutex);
                Running = &Generator;
                RunningRules = &RulesFile;
                Deadline = std::chrono::steady_clock::now() + Timeout;
                Signal.notify_all();
            }

            void End()
            {
                const std::lock_guard Lock(Mutex);
                Running = nullptr;
            }

        private:

            void Watch()
            {
                std::unique_lock Lock(Mutex);
                while (!bStopping)
                {
                    if (Running == nullptr)
                    {
                        Signal.wait(Lock);
                        continue;
                    }
                    Signal.wait_until(Lock, Deadline);
                    if (Running != nullptr && std::chrono::steady_clock::now() >= Deadline)
                    {
                        const std::string Name(Running->GetName());
                        FDiagnostics::Get().Errorf({ *RunningRules, 1 }, EDiagId::GeneratorDiagnostic,
                            "Generator '%s' ran past its %lld second limit, so the run stopped. Its outputs were discarded and the built-in outputs are complete.",
                            Name.c_str(), static_cast<long long>(Timeout.count()));
                        FDiagnostics::Get().PrintSummary();
                        std::_Exit(1);
                    }
                }
            }

            std::mutex                            Mutex;
            std::condition_variable               Signal;
            std::chrono::seconds                  Timeout;
            std::chrono::steady_clock::time_point Deadline;
            const IReflectionGenerator*           Running = nullptr;
            const std::string*                    RunningRules = nullptr;
            bool                                  bStopping = false;
            std::thread                           Thread;
        };
    }

    void FGeneratorDiagnostics::Error(FSourceLocation Location, const char* Fmt, ...) const
    {
        va_list Args;
        va_start(Args, Fmt);
        Report(true, Location, Fmt, Args);
        va_end(Args);
    }

    void FGeneratorDiagnostics::Warning(FSourceLocation Location, const char* Fmt, ...) const
    {
        va_list Args;
        va_start(Args, Fmt);
        Report(false, Location, Fmt, Args);
        va_end(Args);
    }

    void FGeneratorDiagnostics::Report(bool bError, FSourceLocation Location, const char* Fmt, va_list Args) const
    {
        char Message[2048];
        std::vsnprintf(Message, sizeof(Message), Fmt, Args);

        const FDiagLocation Loc{ Model.String(Location.File), Location.Line };
        const std::lock_guard Lock(Mutex);
        if (bError)
        {
            FDiagnostics::Get().Errorf(Loc, EDiagId::GeneratorDiagnostic, "%s", Message);
        }
        else
        {
            FDiagnostics::Get().Warningf(Loc, EDiagId::GeneratorDiagnostic, "%s", Message);
        }
    }

    FCodeWriter& FGeneratedOutputs::Open(const std::string& Path)
    {
        const std::lock_guard Lock(Mutex);
        std::unique_ptr<FCodeWriter>& Writer = Files[Path];
        if (Writer == nullptr)
        {
            Writer = std::make_unique<FCodeWriter>();
        }
        return *Writer;
    }

    void FGeneratedOutputs::Remove(const std::string& Path)
    {
        const std::lock_guard Lock(Mutex);
        Removed.push_back(Path);
    }

    std::vector<std::string> FGeneratedOutputs::GetPaths() const
    {
        const std::lock_guard Lock(Mutex);
        std::vector<std::string> Paths(Removed);
        for (const auto& Entry : Files)
        {
            Paths.push_back(Entry.first);
        }
        return Paths;
    }

    void FGeneratedOutputs::Flush(FWorkerPool& Workers)
    {
        std::vector<const std::pair<const std::string, std::unique_ptr<FCodeWriter>>*> Written;
        for (const auto& Entry : Files)
        {
            Written.push_back(&Entry);
        }
        Workers.ParallelFor(Written.size(), [&Written](size_t Index)
        {
            WriteFileIfChanged(Written[Index]->first, Written[Index]->second->String());
        });
        for (const std::string& Path : Removed)
        {
            if (Files.find(Path) == Files.end())
            {
                std::error_code Ec;
                std::filesystem::remove(std::filesystem::path(Path.c_str()), Ec);
            }
        }
        Files.clear();
        Removed.clear();
    }

    FWorkerPool::FWorkerPool(size_t NumThreads)
    {
        for (size_t Index = 0; Index < NumThreads; ++Index)
        {
            Threads.emplace_back([this] { WorkerLoop(); });
        }
    }

    FWorkerPool::~FWorkerPool()
    {
        {
            const std::lock_guard Lock(Mutex);
            bStopping = true;
        }
        Signal.notify_all();
        for (std::thread& Thread : Threads)
        {
            Thread.join();
        }
    }

    bool FWorkerPool::RunQueuedItem()
    {
        std::shared_ptr<FBatch> Batch;
        size_t Index = 0;
        {
            const std::lock_guard Lock(Mutex);
            while (!Batches.empty())
            {
                Batch = Batches.front();
                Index = Batch->Next.fetch_add(1, std::memory_order_relaxed);
                if (Index < Batch->Count)
                {
                    break;
                }
                Batches.pop_front();
                Batch.reset();
            }
        }
        if (Batch == nullptr)
        {
            return false;
        }

        try
        {
            auto Item = [&] { (*Batch->Body)(Index); };
            RunCatchingFaults(Item);
        }
        catch (...)
        {
            std::call_once(Batch->FailureOnce, [&] { Batch->Failure = std::current_exception(); });
        }
        if (Batch->Done.fetch_add(1, std::memory_order_acq_rel) + 1 == Batch->Count)
        {
            const std::lock_guard Lock(Mutex);
            Signal.notify_all();
        }
        return true;
    }

    void FWorkerPool::WorkerLoop()
    {
        while (true)
        {
            if (RunQueuedItem())
            {
                continue;
            }
            std::unique_lock Lock(Mutex);
            Signal.wait(Lock, [this] { return bStopping || !Batches.empty(); });
            if (bStopping && Batches.empty())
            {
                return;
            }
        }
    }

    void FWorkerPool::ParallelFor(size_t Count, const std::function<void(size_t)>& Body)
    {
        if (Count == 0)
        {
            return;
        }

        const auto Batch = std::make_shared<FBatch>();
        Batch->Body = &Body;
        Batch->Count = Count;
        {
            const std::lock_guard Lock(Mutex);
            Batches.push_back(Batch);
        }
        Signal.notify_all();

        while (Batch->Done.load(std::memory_order_acquire) < Count)
        {
            if (!RunQueuedItem())
            {
                std::unique_lock Lock(Mutex);
                Signal.wait(Lock, [&] { return Batch->Done.load(std::memory_order_acquire) == Count || !Batches.empty(); });
            }
        }
        if (Batch->Failure)
        {
            std::rethrow_exception(Batch->Failure);
        }
    }

    FGeneratorRunner::FGeneratorRunner()
    {
        Generators.push_back(std::make_unique<FCppReflectionGenerator>());
        Generators.push_back(std::make_unique<FCSharpBindingGenerator>());
        for (const auto& Generator : Generators)
        {
            Claim(*Generator, {});
        }
    }

    void FGeneratorRunner::Claim(IReflectionGenerator& Generator, const FDiagLocation& Where)
    {
        const std::string Name(Generator.GetName());

        // Claimed apart first, so a name another generator already owns is reported rather than silently shared.
        FSpecifierRegistry Claims;
        Generator.ClaimSpecifiers(Claims);
        for (size_t Index = 0; Index < static_cast<size_t>(ESpecifierTarget::Size); ++Index)
        {
            const ESpecifierTarget Target = static_cast<ESpecifierTarget>(Index);
            for (const FSpecifierInfo& Info : Claims.GetClaimed(Target))
            {
                if (!Specifiers.Claim(Target, Info))
                {
                    FDiagnostics::Get().Errorf(Where, EDiagId::GeneratorDiagnostic, "Generator '%s' claims %s specifier '%.*s', which another generator already claimed.",
                        Name.c_str(), SpecifierTargetToString(Target), static_cast<int>(Info.Name.size()), Info.Name.data());
                }
            }
        }
    }

    void FGeneratorRunner::Load(const FReflectorGeneratorInput& Input)
    {
        const FDiagLocation Where{ Input.RulesFile, 1 };

        // Never unloaded, since the specifier names it claims point into it.
        #if defined(_WIN32)
            const HMODULE Library = LoadLibraryW(std::filesystem::path(Input.Library.c_str()).c_str());
            const auto Find = [Library](const char* Symbol) { return Library != nullptr ? reinterpret_cast<void*>(GetProcAddress(Library, Symbol)) : nullptr; };
        #else
            void* const Library = dlopen(Input.Library.c_str(), RTLD_NOW | RTLD_LOCAL);
            const auto Find = [Library](const char* Symbol) { return Library != nullptr ? dlsym(Library, Symbol) : nullptr; };
        #endif

        const auto ApiVersion = reinterpret_cast<FGeneratorApiVersionFn>(Find(kGeneratorApiVersionSymbol));
        const auto Create = reinterpret_cast<FCreateGeneratorFn>(Find(kCreateGeneratorSymbol));
        if (ApiVersion == nullptr || Create == nullptr)
        {
            FDiagnostics::Get().Errorf(Where, EDiagId::GeneratorDiagnostic, "Generator library '%s' is missing or exports no LUMINA_REFLECTION_GENERATOR entry points.",
                Input.Library.c_str());
            return;
        }
        if (ApiVersion() != kReflectionGeneratorApiVersion)
        {
            FDiagnostics::Get().Errorf(Where, EDiagId::GeneratorDiagnostic, "Generator library '%s' targets reflection generator API %u, but this Reflector implements %u. Rebuild it against this Reflector.",
                Input.Library.c_str(), ApiVersion(), kReflectionGeneratorApiVersion);
            return;
        }

        FLoadedGenerator Loading{ nullptr, Input.RulesFile };
        const std::string Failure = RunIsolated([&]
        {
            Loading.Generator.reset(Create(Input.Options));
            Claim(*Loading.Generator, Where);
        });
        if (!Failure.empty())
        {
            FDiagnostics::Get().Errorf(Where, EDiagId::GeneratorDiagnostic, "Generator library '%s' failed while loading: %s.", Input.Library.c_str(), Failure.c_str());
            return;
        }
        std::printf("Loaded generator '%.*s' from %s\n", static_cast<int>(Loading.Generator->GetName().size()), Loading.Generator->GetName().data(), Input.Library.c_str());
        Loaded.push_back(std::move(Loading));
    }

    void FGeneratorRunner::Run(const FReflectionModel& Model, std::chrono::seconds Timeout)
    {
        const unsigned Hardware = std::thread::hardware_concurrency();
        FWorkerPool Workers(Hardware > 1 ? Hardware - 1 : 0);
        const FGeneratorDiagnostics Diagnostics(Model);
        const FRunContext Context{ Model, Specifiers, Diagnostics, Workers };

        // Every path written so far, which a loaded generator may not overwrite or remove.
        std::mutex WrittenMutex;
        std::unordered_set<std::string> Written;
        const auto Commit = [&](FGeneratedOutputs& Outputs)
        {
            if (!Loaded.empty())
            {
                std::vector<std::string> Paths = Outputs.GetPaths();
                const std::lock_guard Lock(WrittenMutex);
                Written.insert(std::make_move_iterator(Paths.begin()), std::make_move_iterator(Paths.end()));
            }
            Outputs.Flush(Workers);
        };

        for (const auto& Generator : Generators)
        {
            Generator->BeginRun(Context);
        }

        std::vector<FModuleView> Modules;
        for (const FModuleView Module : Model.Modules())
        {
            if (!Module.IsReferenceOnly())
            {
                Modules.push_back(Module);
            }
        }

        // Each module writes only its own files, so modules fan out while generators run in order within one.
        Workers.ParallelFor(Modules.size(), [&](size_t Index)
        {
            FGeneratedOutputs Outputs;
            const FModuleContext ModuleContext{ Context, Modules[Index], Outputs };
            for (const auto& Generator : Generators)
            {
                Generator->GenerateModule(ModuleContext);
            }
            Commit(Outputs);
        });

        FGeneratedOutputs Outputs;
        for (const auto& Generator : Generators)
        {
            Generator->EndRun(Context, Outputs);
        }
        Commit(Outputs);

        if (Loaded.empty())
        {
            return;
        }

        // Loaded generators run one at a time after the built-in outputs are on disk, so none can leave those half written.
        FGeneratorWatchdog Watchdog(Timeout);
        for (const FLoadedGenerator& Entry : Loaded)
        {
            IReflectionGenerator& Generator = *Entry.Generator;
            const FDiagLocation Where{ Entry.RulesFile, 1 };
            const uint32_t ErrorsBefore = FDiagnostics::Get().GetErrorCount();

            // One set per module and a last for EndRun, written only once the generator finished cleanly.
            std::vector<FGeneratedOutputs> Staged(Modules.size() + 1);

            Watchdog.Begin(Generator, Entry.RulesFile);
            std::string Failure = RunIsolated([&]
            {
                Generator.BeginRun(Context);
                Workers.ParallelFor(Modules.size(), [&](size_t Index)
                {
                    Generator.GenerateModule({ Context, Modules[Index], Staged[Index] });
                });
                Generator.EndRun(Context, Staged.back());
            });
            Watchdog.End();

            for (const FGeneratedOutputs& Outputs : Staged)
            {
                for (const std::string& Path : Outputs.GetPaths())
                {
                    if (Failure.empty() && Written.contains(Path))
                    {
                        Failure = "it writes '" + Path + "', which another generator already wrote";
                    }
                }
            }
            if (Failure.empty() && FDiagnostics::Get().GetErrorCount() != ErrorsBefore)
            {
                Failure = "it reported errors";
            }

            if (!Failure.empty())
            {
                const std::string Name(Generator.GetName());
                FDiagnostics::Get().Errorf(Where, EDiagId::GeneratorDiagnostic, "Generator '%s' failed, so its outputs were discarded: %s.", Name.c_str(), Failure.c_str());
                continue;
            }
            for (FGeneratedOutputs& Outputs : Staged)
            {
                Commit(Outputs);
            }
        }
    }
}
