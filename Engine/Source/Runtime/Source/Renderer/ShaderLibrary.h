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

        // Strong references only -- one per owning CMaterial stage. Caches (FResolvedSurface,
        // FDrawBatchKey, FBatch, FGraphicsPipelineKey) hold WEAK FShaderH and are deliberately not counted:
        // an entry is content-keyed and therefore shared, so freeing it when one owner recompiles would
        // break every other owner. Weak holders find out via a stale handle and re-resolve.
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
                .Source     = TSpan<const std::byte>(reinterpret_cast<const std::byte*>(Spirv.data()), Spirv.size() * sizeof(uint32)),
                .EntryPoint = "main"
            };
        }
    };

    class RUNTIME_API FShaderLibrary
    {
    public:

        FShaderLibrary();
        ~FShaderLibrary();

        /** Fetches (compiling on demand) a shader by name -- "TexturePaint.slang" -- or by full virtual
         *  path -- "/Game/Shaders/GameOfLife.slang". A name is resolved against the ordered shader roots
         *  (engine, plugins, project, module-registered; see Shaders::GetSearchRoots), so a game or
         *  plugin shader is fetched exactly like an engine one. Use the full path when two roots ship the
         *  same file name -- the bare name resolves to whichever root comes first. */
        static FShaderH Get(const FName& NameOrPath, TSpan<const FString> Defines = {});

        /** Interns Spirv by CONTENT and returns a handle with one strong reference taken. The caller owns
         *  that reference and must AddRef/Release it; identical bytecode returns the SAME handle, which is
         *  what collapses material instances into one FDrawBatchKey and therefore one draw. */
        static FShaderH Commit(const FName& Key, ERHIShaderType Type, TSpan<const uint32> Spirv);

        /** Checked deref. Null when the entry was freed since Handle was minted -- the signal that whatever
         *  cached it must re-resolve. Never dereference a handle any other way. */
        static const FShaderEntry* Resolve(FShaderH Handle);

        static bool IsLive(FShaderH Handle) { return Resolve(Handle) != nullptr; }

        static void AddRef(FShaderH Handle);

        /** Drops one strong reference. At zero the entry is queued for release at the next frame boundary,
         *  never freed inline: readers deref handles lock-free, so a free racing a lookup is only safe at a
         *  point where no lookup is in flight. */
        static void Release(FShaderH Handle);

        /** Frees everything Release queued. Call between frames, with no shader lookup in flight. */
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

        /** Name or path -> the virtual path an entry is keyed on. Consults PathsByName first (the only
         *  thing a packaged build can use -- it has no shader source to search), then the shader roots. */
        static FName CanonicalPath(const FName& NameOrPath);

        /** Records Path under its bare file name so a later Get("Foo.slang") finds it. Caller holds Mutex. */
        void IndexShaderName(const FName& Path);

        FMutex                      Mutex;
        // Boxed in a segment map rather than a hash map of raw pointers: the map owns the generation bump
        // on release, which is the entire safety story for weak handles.
        TSegmentMap<FShaderEntry>   Entries;
        THashMap<uint64, FShaderH>  HandlesByHash;
        // Bare file name -> full virtual path, filled as shaders commit. First root to ship a name wins.
        THashMap<FName, FName>      PathsByName;
        TVector<FShaderH>           PendingRelease;
        uint32                      NextID = 1;
    };
}
