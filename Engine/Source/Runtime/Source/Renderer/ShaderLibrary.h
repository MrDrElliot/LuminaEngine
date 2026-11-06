#pragma once

#include "RHI.h"
#include "RenderResource.h"
#include "Shader.h"
#include "Containers/Name.h"
#include "Containers/HashTable.h"
#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Core/Threading/Thread.h"
#include "Containers/SegmentArray.h"
#include "ShaderHandle.h"

namespace Lumina
{
    struct FShaderEntry
    {
        FName            Path;
        TVector<FString> Defines;
        ERHIShaderType   Type = ERHIShaderType::None;
        TVector<uint32>  Spirv;
        uint32           ID = 0;          // process-unique, never reused
        uint64           MapHash = 0;     // key into HandlesByHash, so a release can unmap itself
        uint32           Generation = 0;  // 0 = not compiled yet; bumps on every (re)commit

        // One per owning CMaterial stage; caches hold weak handles and re-resolve when one goes stale.
        uint32           RefCount = 0;

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
                .Source     = TSpan<const std::byte>(reinterpret_cast<const std::byte*>(Spirv.GetData()), Spirv.Num() * sizeof(uint32)),
                .EntryPoint = "main",
                .DebugName  = *Path
            };
        }
    };

    class RUNTIME_API FShaderLibrary
    {
    public:

        FShaderLibrary();
        ~FShaderLibrary();
        FShaderLibrary(const FShaderLibrary&) = delete;
        FShaderLibrary& operator=(const FShaderLibrary&) = delete;
        FShaderLibrary(FShaderLibrary&&) = delete;
        FShaderLibrary& operator=(FShaderLibrary&&) = delete;

        // A bare name resolves against the first of Shaders::GetSearchRoots to ship it, so pass the full path when two roots share it.
        static FShaderH Get(const FName& NameOrPath, TSpan<const FString> Defines = {});

        // Interned by content, so identical bytecode returns the same handle with one strong reference the caller owns.
        static FShaderH Commit(const FName& Key, ERHIShaderType Type, TSpan<const uint32> Spirv);

        // The only safe deref; null means the entry was freed since Handle was minted and the caller must re-resolve.
        static const FShaderEntry* Resolve(FShaderH Handle);

        static bool IsLive(FShaderH Handle) { return Resolve(Handle) != nullptr; }

        static void AddRef(FShaderH Handle);

        // At zero the entry is queued rather than freed, because readers deref handles lock-free.
        static void Release(FShaderH Handle);

        // Call between frames, with no shader lookup in flight.
        static void FlushPendingReleases();

        // Compiler-callback commit for engine shader files (keyed by DebugName + Defines).
        static void Commit(const FShaderHeader& Header);

#if USING(WITH_EDITOR)
        static FShaderEntry::FGPUStats GetGPUStats(FShaderH Handle);

        static void PublishPipelineStats(FShaderH Handle, TVector<RHI::FPipelineStat>&& Stats);

        static bool HasPipelineStats(FShaderH Handle);
#endif

    private:

        FShaderH FindOrCreate(uint64 Hash);

        // Consults PathsByName before the shader roots, since a packaged build has no shader source to search.
        static FName CanonicalPath(const FName& NameOrPath);

        // Caller holds Mutex.
        void IndexShaderName(const FName& Path);

        FMutex                      Mutex;
        // A segment map owns the generation bump on release, which is what keeps weak handles safe.
        TSegmentMap<FShaderEntry>   Entries;
        THashMap<uint64, FShaderH>  HandlesByHash;
        // Bare file name -> full virtual path, filled as shaders commit. First root to ship a name wins.
        THashMap<FName, FName>      PathsByName;
        TVector<FShaderH>           PendingRelease;
        uint32                      NextID = 1;
    };
}
