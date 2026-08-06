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

#if USING(WITH_EDITOR)
        // Post-compile GPU diagnostics, surfaced by the material editor's Shader Stats panel. Both of
        // these are cliffs that are invisible in the graph and only show up in a GPU capture, which is
        // why they are worth carrying: a material author has no other way to see them.
        //
        // Written from the render thread (pipeline creation), read from the game thread (editor UI), so
        // go through FShaderLibrary::GetGPUStats / PublishPipelineStats rather than touching this
        // directly -- those copy under the library mutex.
        struct FGPUStats
        {
            // Count of function-scope OpVariables whose type is an ARRAY. These do NOT promote to
            // registers: the driver backs them with LOCAL memory, so every indexed access becomes an
            // LDL/STL and stalls on the long scoreboard. Slang emits them for loop-indexed locals even
            // under [unroll], so a material can pick these up without anything in the graph looking wrong.
            uint32 LocalArrayCount   = 0;
            uint32 LocalArrayScalars = 0;   // total scalar slots across those arrays

            // Driver-reported per-stage statistics, chiefly the register count that decides which
            // occupancy step the shader lands on. Empty until a pipeline using this shader is created,
            // and always empty without VK_KHR_pipeline_executable_properties.
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

        // Stable entry for an engine shader file; compiles synchronously if the startup batch
        // hasn't delivered it yet. Never returns null: cache the pointer (it lives for the
        // process) and check IsValid() per use - false only means compilation failed.
        static const FShaderEntry* Get(const FName& Path, TSpan<const FString> Defines = {});

        // Create or in-place refresh an entry from externally produced bytecode
        // (graph-compiled materials / particle systems). Same key = same entry.
        static const FShaderEntry* Commit(const FName& Key, ERHIShaderType Type, TSpan<const uint32> Spirv);

        // Compiler-callback commit for engine shader files (keyed by DebugName + Defines).
        static void Commit(const FShaderHeader& Header);

#if USING(WITH_EDITOR)
        // Snapshot of an entry's GPU diagnostics, copied under the library mutex. Pipeline stats are
        // published from the render thread while the material editor reads them from the game thread,
        // so neither side may touch FShaderEntry::GPUStats directly.
        static FShaderEntry::FGPUStats GetGPUStats(const FShaderEntry* Entry);

        // Record driver statistics for a shader the first time a pipeline using it is created. A no-op
        // if this entry already has stats for its current Generation -- pipelines are created per
        // permutation and per pass, and they would all report the same numbers for the same bytecode.
        static void PublishPipelineStats(const FShaderEntry* Entry, TVector<RHI::FPipelineStat>&& Stats);

        // Whether PublishPipelineStats has already run for this entry. Cheap enough to call on every
        // pipeline creation, which is the point: it keeps the driver query off the steady-state path.
        static bool HasPipelineStats(const FShaderEntry* Entry);
#endif

    private:

        FShaderEntry& FindOrCreate(uint64 Hash);

        FMutex                          Mutex;
        THashMap<uint64, FShaderEntry*> Entries;
        uint32                          NextID = 1;
    };
}
