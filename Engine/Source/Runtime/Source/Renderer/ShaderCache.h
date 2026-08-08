#pragma once

#include "Shader.h"
#include "Containers/Array.h"
#include "Containers/String.h"

namespace Lumina
{
    namespace FShaderCache
    {
        constexpr uint32 SHADER_CACHE_VERSION = 20;

        constexpr const char* CACHE_DIR = "/Intermediates/ShaderCache";

        uint64 ComputeSourceSetHash(FStringView ShaderVirtualPath, const TVector<FString>& Defines);

        // Stable cache filename for (shader path + defines), independent of disk layout.
        FString CachePathFor(FStringView ShaderVirtualPath, const TVector<FString>& Defines);

        bool TryLoad(FStringView ShaderVirtualPath, const TVector<FString>& Defines, uint64 SourceHash, FShaderHeader& OutHeader);

        bool TryLoadByCachePath(FStringView CacheVirtualPath, uint64 SourceHash, FShaderHeader& OutHeader);

        // Atomic write under CACHE_DIR. Creates the directory if missing.
        bool Save(FStringView ShaderVirtualPath, const TVector<FString>& Defines, uint64 SourceHash, const FShaderHeader& Header);
    }
}
