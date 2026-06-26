#pragma once

#include "Shader.h"
#include "Containers/Array.h"
#include "Containers/String.h"

namespace Lumina
{
    // SPIR-V cache living under /Engine/Intermediate/ShaderCache (editor-writable;
    // bundled into .pak by the cooker so packaged builds skip Slang entirely).
    namespace FShaderCache
    {
        // Bump when the .lsc binary layout or compile pipeline changes in a way
        // that invalidates older entries (e.g. Slang upgrade, header layout).
        // v2: ERHIShaderType renumbered when the old RHI's resource-type enum was trimmed.
        // v3: shader debug-info level raised to STANDARD (Nsight source debugging) on non-AMD non-Shipping.
        // v4: Slang optimization forced to HIGH (DEFAULT -O1 emitted spirv-val-invalid BDA pointer locals).
        // v5: deferred material binning re-keyed per-pixel on owning SLOT (classify/DeferredMaterial) so
        //     instances of a shared master shade in one draw.
        // v6: SampleTexture2DGrad added (GlobalRHI); deferred material lane samples with analytic UV gradients
        //     (correct mips across triangle/meshlet/instance seams -- the VisBuffer deferred-texturing fix).
        // v7: VisBuffer geometry unified opaque+masked via the VISBUFFER_MASKED spec constant (FVisVertexOut
        //     interpolants gated/dead-stripped); separate masked geometry shaders removed.
        // v8: VisBuffer mesh shader sized to the vertex phase ([numthreads(64)] + grid-stride tri loop);
        //     BuildCullDispatchArgs added (late cull now indirect-dispatched from DeferCount).
        // v9: mesh shaders (MeshletVisBuffer/MeshletMesh) take SV_DrawIndex + pull MeshletBase from the cull
        //     args (one indirect draw per batch, not per sub-draw) -- push-constant layout changed.
        constexpr uint32 SHADER_CACHE_VERSION = 9;

        constexpr const char* CACHE_DIR = "/Intermediates/ShaderCache";

        // Hash of the main .slang source, every resolvable transitive import/include, and the
        // sorted define list. Returns 0 if the main source can't be read.
        uint64 ComputeSourceSetHash(FStringView ShaderVirtualPath, const TVector<FString>& Defines);

        // Stable cache filename for (shader path + defines), independent of disk layout.
        FString CachePathFor(FStringView ShaderVirtualPath, const TVector<FString>& Defines);

        // Hit only if file exists, magic/version match, and SourceHash matches the stored one.
        // SourceHash == 0 disables the check (packaged builds ship no source -- trust the cache).
        bool TryLoad(FStringView ShaderVirtualPath, const TVector<FString>& Defines, uint64 SourceHash, FShaderHeader& OutHeader);

        // Same as TryLoad but loads directly from a known cache file path.
        // SourceHash == 0 disables the check.
        bool TryLoadByCachePath(FStringView CacheVirtualPath, uint64 SourceHash, FShaderHeader& OutHeader);

        // Atomic write under CACHE_DIR. Creates the directory if missing.
        bool Save(FStringView ShaderVirtualPath, const TVector<FString>& Defines, uint64 SourceHash, const FShaderHeader& Header);
    }
}
