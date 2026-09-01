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
        uint64 ComputeSourceSetHash(FStringView ShaderVirtualPath, const TVector<FString>& Defines, const TVector<FString>& SearchRoots, FStringView EntryPoint);

        FString CachePathFor(FStringView ShaderVirtualPath, const TVector<FString>& Defines, FStringView EntryPoint);

        bool TryLoad(FStringView ShaderVirtualPath, const TVector<FString>& Defines, FStringView EntryPoint, uint64 SourceHash, FShaderHeader& OutHeader);

        bool TryLoadByCachePath(FStringView CacheVirtualPath, uint64 SourceHash, FShaderHeader& OutHeader);

        bool Save(FStringView ShaderVirtualPath, const TVector<FString>& Defines, FStringView EntryPoint, uint64 SourceHash, const FShaderHeader& Header);

        // A generated source has no file to key on, so the key is its text plus what it includes.
        uint64 ComputeRawSourceHash(FStringView Source, const TVector<FString>& Defines, const TVector<FString>& SearchRoots, FStringView TemplateVirtualPath, FStringView EntryPoint);

        bool TryLoadRaw(uint64 KeyHash, FShaderHeader& OutHeader);

        bool SaveRaw(uint64 KeyHash, const FShaderHeader& Header);
    }
}
