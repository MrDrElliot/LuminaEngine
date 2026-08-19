#pragma once

#include "Shader.h"
#include "Containers/Vector.h"
#include "Containers/String.h"

namespace Lumina
{
    namespace FShaderCache
    {
        // 21: FMeshletVertex/FMeshletSkinnedVertex/FPreSkinnedVertex gained a UV1 field (TEXCOORD_1).
        // 22: GBuffer flags byte carries a 3-bit shading model; Clearcoat borrows B.a and C.b.
        // 23: shader identity is the full virtual path, not the file name (multi-root shader search).
        constexpr uint32 SHADER_CACHE_VERSION = 23;

        constexpr const char* CACHE_DIR = "/Intermediates/ShaderCache";

        // SearchRoots resolve `#include`/`import` the same way the compile session does; see
        // Shaders::GetSearchRoots. Returns 0 -- "do not cache" -- when the source cannot be read.
        uint64 ComputeSourceSetHash(FStringView ShaderVirtualPath, const TVector<FString>& Defines, const TVector<FString>& SearchRoots);

        // Stable cache filename for (shader path + defines), independent of disk layout.
        FString CachePathFor(FStringView ShaderVirtualPath, const TVector<FString>& Defines);

        bool TryLoad(FStringView ShaderVirtualPath, const TVector<FString>& Defines, uint64 SourceHash, FShaderHeader& OutHeader);

        bool TryLoadByCachePath(FStringView CacheVirtualPath, uint64 SourceHash, FShaderHeader& OutHeader);

        // Atomic write under CACHE_DIR. Creates the directory if missing.
        bool Save(FStringView ShaderVirtualPath, const TVector<FString>& Defines, uint64 SourceHash, const FShaderHeader& Header);
    }
}
