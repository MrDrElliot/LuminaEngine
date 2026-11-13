#pragma once

#include <atomic>
#include <cstdarg>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Reflector/Public/CodeWriter.h"
#include "Reflector/Public/ReflectionModel.h"
#include "Reflector/Public/SpecifierRegistry.h"

namespace Lumina::Reflection
{
    // Bumped on any breaking change to this header or the model it exposes.
    inline constexpr uint32_t kReflectionGeneratorApiVersion = 1;

    // Reports through the Reflector's diagnostics as file(line), and any error fails the build.
    class FGeneratorDiagnostics
    {
    public:

        explicit FGeneratorDiagnostics(const FReflectionModel& InModel) : Model(InModel) {}

        LRT_API void Error(FSourceLocation Location, const char* Fmt, ...) const;
        LRT_API void Warning(FSourceLocation Location, const char* Fmt, ...) const;

    private:

        LRT_API void Report(bool bError, FSourceLocation Location, const char* Fmt, va_list Args) const;

        const FReflectionModel& Model;
        mutable std::mutex      Mutex;
    };

    // The run's worker threads. A caller works through the queue too, so a nested ParallelFor never waits on threads it occupies.
    class FWorkerPool
    {
    public:

        explicit FWorkerPool(size_t NumThreads);
        ~FWorkerPool();

        // Runs Body for every index below Count, returning once all finished and rethrowing the first item's failure. Thread-safe.
        LRT_API void ParallelFor(size_t Count, const std::function<void(size_t)>& Body);

    private:

        struct FBatch
        {
            const std::function<void(size_t)>* Body = nullptr;
            size_t                              Count = 0;
            std::atomic<size_t>                 Next{ 0 };
            std::atomic<size_t>                 Done{ 0 };
            std::once_flag                      FailureOnce;
            std::exception_ptr                  Failure;
        };

        // Runs one unclaimed item of the oldest unfinished batch, false when nothing is left to claim.
        bool RunQueuedItem();
        void WorkerLoop();

        std::mutex                          Mutex;
        std::condition_variable             Signal;
        std::deque<std::shared_ptr<FBatch>> Batches;
        std::vector<std::thread>            Threads;
        bool                                bStopping = false;
    };

    // The files one module or the run produces, each written at the end only when its content changed.
    class FGeneratedOutputs
    {
    public:

        // The writer for Path, shared by every generator naming it, so their fragments append in generator order. Thread-safe.
        LRT_API FCodeWriter& Open(const std::string& Path);

        // Deletes Path once generation ends, unless something opened it. Thread-safe.
        LRT_API void Remove(const std::string& Path);

        // Every path opened or removed so far.
        std::vector<std::string> GetPaths() const;

        // Writes every opened file whose content changed and deletes every removed one that nothing opened.
        void Flush(FWorkerPool& Workers);

    private:

        mutable std::mutex                                            Mutex;
        std::unordered_map<std::string, std::unique_ptr<FCodeWriter>> Files;
        std::vector<std::string>                                      Removed;
    };

    struct FRunContext
    {
        const FReflectionModel&      Model;
        const FSpecifierRegistry&    Specifiers;
        const FGeneratorDiagnostics& Diagnostics;
        FWorkerPool&                 Workers;
    };

    struct FModuleContext
    {
        const FRunContext& Run;
        FModuleView        Module;
        FGeneratedOutputs& Outputs;
    };

    class IReflectionGenerator
    {
    public:

        virtual ~IReflectionGenerator() = default;

        virtual std::string_view GetName() const = 0;

        // Claims the specifiers and metadata keys this generator reads, before anything is parsed.
        virtual void ClaimSpecifiers(FSpecifierRegistry& Registry) { (void)Registry; }

        // Once per run on the calling thread, before any module.
        virtual void BeginRun(const FRunContext& Context) { (void)Context; }

        // Once per module that is not reference-only, concurrently with other modules.
        virtual void GenerateModule(const FModuleContext& Context) = 0;

        // Once per run on the calling thread, after every module was generated and flushed.
        virtual void EndRun(const FRunContext& Context, FGeneratedOutputs& Outputs) { (void)Context; (void)Outputs; }
    };

    // The key and value pairs a Reflection module's Build.cs sets in ReflectionGeneratorOptions.
    using FGeneratorOptions = std::vector<std::pair<std::string, std::string>>;

    using FGeneratorApiVersionFn = uint32_t (*)();
    using FCreateGeneratorFn = IReflectionGenerator* (*)(const FGeneratorOptions& Options);

    inline constexpr const char* kGeneratorApiVersionSymbol = "LuminaReflectionGeneratorApiVersion";
    inline constexpr const char* kCreateGeneratorSymbol = "LuminaCreateReflectionGenerator";
}

// Exports the entry points the Reflector loads a Reflection module through, once in one of its source files.
#define LUMINA_REFLECTION_GENERATOR(GeneratorType) \
    extern "C" LRT_EXPORT uint32_t LuminaReflectionGeneratorApiVersion() { return ::Lumina::Reflection::kReflectionGeneratorApiVersion; } \
    extern "C" LRT_EXPORT ::Lumina::Reflection::IReflectionGenerator* LuminaCreateReflectionGenerator(const ::Lumina::Reflection::FGeneratorOptions& Options) \
    { \
        return new GeneratorType(Options); \
    }
