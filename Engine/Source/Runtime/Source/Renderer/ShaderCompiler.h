#pragma once

#include "Shader.h"
#include "Containers/Array.h"
#include "Containers/Function.h"
#include "Containers/String.h"
#include "Core/Threading/Atomic.h"
#include "Core/Threading/Thread.h"

namespace Lumina
{
    class FShaderLibrary;
    class IShaderCompiler;

    // Global shader compiler + library, owned by FRenderManager.
    extern RUNTIME_API IShaderCompiler* GShaderCompiler;
    extern RUNTIME_API FShaderLibrary*  GShaderLibrary;

    struct FShaderCompileOptions
    {
        bool bGenerateReflectionData = true;
        TVector<FString> MacroDefinitions;
        // Human-readable name for this shader: used as the Slang source path (so crash dumps / Aftermath
        // map to "<DebugName>.slang:line" instead of the generic "RawShader") and the registered debug name.
        FString DebugName = "RawShader";
    };
    
    
    class IShaderCompiler
    {
    public:
        using CompletedFunc = TFunction<void(FShaderHeader)>;

        virtual ~IShaderCompiler() = default;

        virtual void Initialize() = 0;
        virtual void Shutdown() = 0;

        virtual bool CompilerShaderRaw(FString ShaderString, const FShaderCompileOptions& CompileOptions, CompletedFunc OnCompleted) = 0;
        virtual bool CompileShaderPath(FString ShaderPath, const FShaderCompileOptions& CompileOptions, CompletedFunc OnCompleted) = 0;
        virtual bool CompileShaderPaths(TSpan<FString> ShaderPaths, TSpan<FShaderCompileOptions> CompileOptions, CompletedFunc OnCompleted) = 0;

        virtual bool HasPendingRequests() const = 0;
        virtual void Flush() const = 0;
        
    };
    
    
    class FSpirVShaderCompiler : public IShaderCompiler
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

        bool CompilerShaderRaw(FString ShaderString, const FShaderCompileOptions& CompileOptions, CompletedFunc OnCompleted) override;
        bool CompileShaderPath(FString ShaderPath, const FShaderCompileOptions& CompileOptions, CompletedFunc OnCompleted) override;
        bool CompileShaderPaths(TSpan<FString> ShaderPaths, TSpan<FShaderCompileOptions> CompileOptions, CompletedFunc OnCompleted) override;

        bool HasPendingRequests() const override;
        void Flush() const override;
        
        
        FMutex                      RequestMutex;
        TAtomic<uint32>             PendingTasks;
    };
}
