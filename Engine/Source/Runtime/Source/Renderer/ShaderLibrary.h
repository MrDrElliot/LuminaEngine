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
    struct FShaderEntry
    {
        FName            Path;
        TVector<FString> Defines;
        ERHIShaderType   Type = ERHIShaderType::None;
        TVector<uint32>  Spirv;
        uint32           ID = 0;          // process-unique, never reused
        uint32           Generation = 0;  // 0 = not compiled yet; bumps on every (re)commit

        bool IsValid() const { return Generation != 0; }

#if USING(WITH_EDITOR)
        struct FGPUStats
        {
            uint32 LocalArrayCount   = 0;
            uint32 LocalArrayScalars = 0;   // total scalar slots across those arrays

            TVector<RHI::FPipelineStat> Pipeline;
        };
        FGPUStats GPUStats;
#endif

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

        static const FShaderEntry* Get(const FName& Path, TSpan<const FString> Defines = {});

        static const FShaderEntry* Commit(const FName& Key, ERHIShaderType Type, TSpan<const uint32> Spirv);

        // Compiler-callback commit for engine shader files (keyed by DebugName + Defines).
        static void Commit(const FShaderHeader& Header);

#if USING(WITH_EDITOR)
        static FShaderEntry::FGPUStats GetGPUStats(const FShaderEntry* Entry);

        static void PublishPipelineStats(const FShaderEntry* Entry, TVector<RHI::FPipelineStat>&& Stats);

        static bool HasPipelineStats(const FShaderEntry* Entry);
#endif

    private:

        FShaderEntry& FindOrCreate(uint64 Hash);

        FMutex                          Mutex;
        THashMap<uint64, FShaderEntry*> Entries;
        uint32                          NextID = 1;
    };
}
