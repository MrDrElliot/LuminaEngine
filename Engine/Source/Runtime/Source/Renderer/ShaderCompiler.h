#pragma once

#include "Shader.h"
#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Containers/Function.h"
#include "Containers/String.h"
#include "Memory/SmartPtr.h"
#include "Core/Threading/Atomic.h"
#include "Core/Threading/Thread.h"
#include "Core/Threading/Sync.h"
#include "TaskSystem/TaskSystem.h"

namespace Lumina
{
    class FShaderLibrary;
    class IShaderCompiler;

    // Global shader compiler + library, owned by FRenderManager.
    namespace Global
    {
        extern RUNTIME_API IShaderCompiler* GShaderCompiler;
        extern RUNTIME_API FShaderLibrary*  GShaderLibrary;
    }

    struct FShaderCompileOptions
    {
        bool bGenerateReflectionData = true;
        TVector<FString> MacroDefinitions;
        FString DebugName = "RawShader";
        // The file a generated source came from, resolving its relative includes for the cache key.
        FString TemplateVirtualPath;
        // Which entry point to compile. Required once a module defines more than one.
        FString EntryPoint;
        // Read by raw compiles, where Background keeps a long compile off any thread only helping out while it waits.
        Jobs::EJobPriority Priority = Jobs::EJobPriority::Normal;
    };
    
    class IShaderCompiler
    {
    public:
        using CompletedFunc = TFunction<void(FShaderHeader)>;
        using FinishedFunc  = TFunction<void()>;

        virtual ~IShaderCompiler() = default;

        virtual void Initialize() = 0;
        virtual void Shutdown() = 0;

        // OnCompleted runs per successful shader, and OnFinished once every request in the call has returned, failed ones included.
        virtual bool CompilerShaderRaw(FString ShaderString, const FShaderCompileOptions& CompileOptions, CompletedFunc OnCompleted, FinishedFunc OnFinished = {}) = 0;
        virtual bool CompileShaderPath(FString ShaderPath, const FShaderCompileOptions& CompileOptions, CompletedFunc OnCompleted, FinishedFunc OnFinished = {}) = 0;
        virtual bool CompileShaderPaths(TSpan<FString> ShaderPaths, TSpan<FShaderCompileOptions> CompileOptions, CompletedFunc OnCompleted, FinishedFunc OnFinished = {}) = 0;

        virtual void Flush() const = 0;
        
    };

    // Waits on the requests handed its Track callbacks, where Flush would also wait on every unrelated compile in flight.
    class RUNTIME_API FShaderCompileWaiter
    {
    public:
        FShaderCompileWaiter();

        // Pass the result as a request's OnFinished, one per call.
        IShaderCompiler::FinishedFunc Track();

        NODISCARD uint32 GetOutstanding() const;
        NODISCARD bool IsSettled() const { return GetOutstanding() == 0; }

        void Wait() const;

    private:
        struct FState
        {
            TAtomic<uint32>         Outstanding{0};
            FMutex             Mutex;
            FConditionVariable Settled;
        };

        // Shared with every callback, since the last one still notifies after Wait may have returned.
        TSharedPtr<FState> State;
    };
    
    // Exported for hosts outside Runtime; Initialize() precompiles the whole VFS shader tree, CompilerShaderRaw does not need it.
    class RUNTIME_API FSpirVShaderCompiler : public IShaderCompiler
    {
    public:
        
        struct FRequest
        {
            FString Path;
            FShaderCompileOptions CompileOptions;
            CompletedFunc OnCompleted;
        };

        FSpirVShaderCompiler();
        void Initialize() override;
        void Shutdown() override;

        bool CompilerShaderRaw(FString ShaderString, const FShaderCompileOptions& CompileOptions, CompletedFunc OnCompleted, FinishedFunc OnFinished = {}) override;
        bool CompileShaderPath(FString ShaderPath, const FShaderCompileOptions& CompileOptions, CompletedFunc OnCompleted, FinishedFunc OnFinished = {}) override;
        bool CompileShaderPaths(TSpan<FString> ShaderPaths, TSpan<FShaderCompileOptions> CompileOptions, CompletedFunc OnCompleted, FinishedFunc OnFinished = {}) override;

        void Flush() const override;

        FMutex                      RequestMutex;
        TAtomic<uint32>             PendingTasks;

        mutable FMutex             PendingMutex;
        mutable FConditionVariable PendingSignal;

    private:

        struct FBatch;

        void CompileChunk(FBatch& Batch, uint32 Start, uint32 End, uint32 Thread);
    };
}
