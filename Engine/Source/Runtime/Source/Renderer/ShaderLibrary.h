#pragma once

#include "RHI.h"
#include "RenderResource.h"
#include "Shader.h"
#include "Containers/Name.h"
#include "Containers/Array.h"
#include "Containers/String.h"
#include "Core/Threading/Thread.h"

namespace Lumina
{
    // One shader in the library. Entries are created on first request and never move or die;
    // a recompile swaps the bytecode in place and bumps Generation, so cached pointers stay
    // valid and pipeline caches keyed on (ID, Generation) pick up new code automatically.
    struct FShaderEntry
    {
        FName            Path;
        TVector<FString> Defines;
        ERHIShaderType   Type = ERHIShaderType::None;
        TVector<uint32>  Spirv;
        uint32           ID = 0;          // process-unique, never reused
        uint32           Generation = 0;  // 0 = not compiled yet; bumps on every (re)commit

        bool IsValid() const { return Generation != 0; }

        // Hash one shader slot of a pipeline key; recompiles change the hash.
        uint64 PipelineHash() const { return ((uint64)ID << 32) | Generation; }

        RHI::FShaderSource Source() const
        {
            return RHI::FShaderSource
            {
                .Source     = TSpan<const std::byte>(reinterpret_cast<const std::byte*>(Spirv.data()), Spirv.size() * sizeof(uint32)),
                .EntryPoint = "main"
            };
        }
    };

    class RUNTIME_API FShaderLibrary
    {
    public:

        ~FShaderLibrary();

        // Stable entry for an engine shader file; compiles synchronously if the startup batch
        // hasn't delivered it yet. Never returns null: cache the pointer (it lives for the
        // process) and check IsValid() per use - false only means compilation failed.
        static const FShaderEntry* Get(const FName& Path, TSpan<const FString> Defines = {});

        // Create or in-place refresh an entry from externally produced bytecode
        // (graph-compiled materials / particle systems). Same key = same entry.
        static const FShaderEntry* Commit(const FName& Key, ERHIShaderType Type, TSpan<const uint32> Spirv);

        // Compiler-callback commit for engine shader files (keyed by DebugName + Defines).
        static void Commit(const FShaderHeader& Header);

    private:

        FShaderEntry& FindOrCreate(uint64 Hash);

        FMutex                          Mutex;
        THashMap<uint64, FShaderEntry*> Entries;
        uint32                          NextID = 1;
    };
}
