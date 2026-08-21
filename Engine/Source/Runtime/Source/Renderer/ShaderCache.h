#pragma once

#include "Shader.h"
#include "Containers/Vector.h"
#include "Containers/String.h"

namespace Lumina
{
    namespace FShaderCache
    {
        constexpr auto kShaderCacheVersion      = 1;
        constexpr FStringView kCacheDirectory   = "/Intermediates/ShaderCache";
        uint64 ComputeSourceSetHash(FStringView ShaderVirtualPath, const TVector<FString>& Defines, const TVector<FString>& SearchRoots);

        FString CachePathFor(FStringView ShaderVirtualPath, const TVector<FString>& Defines);

        bool TryLoad(FStringView ShaderVirtualPath, const TVector<FString>& Defines, uint64 SourceHash, FShaderHeader& OutHeader);

        bool TryLoadByCachePath(FStringView CacheVirtualPath, uint64 SourceHash, FShaderHeader& OutHeader);

        bool Save(FStringView ShaderVirtualPath, const TVector<FString>& Defines, uint64 SourceHash, const FShaderHeader& Header);
    }
}
