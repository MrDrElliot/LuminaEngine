#pragma once

#include "Shader.h"
#include "Containers/Vector.h"
#include "Containers/String.h"

namespace Lumina
{
    namespace FShaderCache
    {
        constexpr auto kShaderCacheVersion      = 2;
        constexpr FStringView kCacheDirectory   = "/Intermediates/ShaderCache";
        uint64 ComputeSourceSetHash(FStringView ShaderVirtualPath, const TVector<FString>& Defines, const TVector<FString>& SearchRoots, FStringView EntryPoint);

        FString CachePathFor(FStringView ShaderVirtualPath, const TVector<FString>& Defines, FStringView EntryPoint);

        bool TryLoad(FStringView ShaderVirtualPath, const TVector<FString>& Defines, FStringView EntryPoint, uint64 SourceHash, FShaderHeader& OutHeader);

        bool TryLoadByCachePath(FStringView CacheVirtualPath, uint64 SourceHash, FShaderHeader& OutHeader);

        bool Save(FStringView ShaderVirtualPath, const TVector<FString>& Defines, FStringView EntryPoint, uint64 SourceHash, const FShaderHeader& Header);

        // A generated source has no file to key on, so the key is its text plus what it includes.
        uint64 ComputeRawSourceHash(FStringView Source, const TVector<FString>& Defines, const TVector<FString>& SearchRoots, FStringView TemplateVirtualPath, FStringView EntryPoint);

        // Material binaries are rebuilt from graphs the project owns, so they live in the project's Intermediates.
        constexpr FStringView kMaterialCacheDirectory = "/ProjectIntermediates/ShaderCache";

        bool TryLoadRaw(uint64 KeyHash, FShaderHeader& OutHeader, FStringView Directory = kCacheDirectory);

        bool SaveRaw(uint64 KeyHash, const FShaderHeader& Header, FStringView Directory = kCacheDirectory);

        bool DeleteRaw(uint64 KeyHash, FStringView Directory);

        // Rewrites one cache file for a cook with its binaries' debug info stripped; false leaves it to ship as it was.
        RUNTIME_API bool StripCacheFileForCook(const TVector<uint8>& Source, TVector<uint8>& Out);
    }
}
