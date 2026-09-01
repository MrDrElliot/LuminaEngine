#include "RuntimePCH.h"
#include "ShaderCache.h"
#include "RenderResource.h"
#include "Core/Math/Hash/Hash.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "FileSystem/FileSystem.h"
#include "Memory/MemoryTracking.h"

namespace Lumina::FShaderCache
{
    namespace
    {
        constexpr uint32 CACHE_MAGIC = 0x3143534C; // 'LSC1'

        FArchive& SerializeBinding(FArchive& Ar, FShaderBinding& B)
        {
            Ar << B.Name;
            Ar << B.Set;
            Ar << B.Binding;
            Ar << B.Size;
            uint8 TypeRaw = (uint8)B.Type;
            Ar << TypeRaw;
            B.Type = (ERHIBindingResourceType)TypeRaw;
            return Ar;
        }

        FArchive& SerializeReflection(FArchive& Ar, FShaderReflection& R)
        {
            uint8 StageRaw = (uint8)R.ShaderType;
            Ar << StageRaw;
            R.ShaderType = (ERHIShaderType)StageRaw;

            size_t Count = Ar.IsReading() ? 0 : R.Bindings.size();
            Ar << Count;
            if (Ar.IsReading())
            {
                R.Bindings.clear();
                R.Bindings.resize(Count);
            }
            for (size_t i = 0; i < Count; ++i)
            {
                SerializeBinding(Ar, R.Bindings[i]);
            }
            return Ar;
        }

        FArchive& SerializeHeader(FArchive& Ar, FShaderHeader& H)
        {
            Ar << H.DebugName;
            Ar << H.Defines;
            Ar << H.Binaries;
            Ar << H.Hash;
            SerializeReflection(Ar, H.Reflection);
            return Ar;
        }

        FString JoinSortedDefines(const TVector<FString>& Defines)
        {
            TVector<FString> Sorted(Defines.begin(), Defines.end());
            Algo::Sort(Sorted);
            FString Joined;
            for (const FString& D : Sorted)
            {
                Joined += D;
                Joined += '\n';
            }
            return Joined;
        }

        FString ResolveInclude(FStringView Token, bool bIsImport, FStringView IncludingDir, const TVector<FString>& SearchRoots)
        {
            FString Candidate;
            if (bIsImport)
            {
                // import path uses dots; map to slashes + .slang.
                Candidate.assign(Token.data(), Token.size());
                for (char& C : Candidate)
                {
                    if (C == '.')
                    {
                        C = '/';
                    }
                }
                Candidate += ".slang";
            }
            else
            {
                Candidate.assign(Token.data(), Token.size());
            }
            
            if (!IncludingDir.empty())
            {
                FString Sibling(IncludingDir.data(), IncludingDir.size());
                if (!Sibling.empty() && Sibling.back() != '/')
                {
                    Sibling += '/';
                }
                Sibling += Candidate;
                if (VFS::Exists(Sibling))
                {
                    return Sibling;
                }
            }

            for (const FString& SearchRoot : SearchRoots)
            {
                FString Root = SearchRoot;
                if (!Root.empty() && Root.back() != '/')
                {
                    Root += '/';
                }
                Root += Candidate;
                if (VFS::Exists(Root))
                {
                    return Root;
                }
            }
            return {};
        }

        FStringView ParentDir(FStringView Path)
        {
            size_t Slash = Path.find_last_of('/');
            if (Slash == FStringView::npos)
            {
                return {};
            }
            return Path.substr(0, Slash);
        }

        bool GatherSourceHash(FStringView VirtualPath, const TVector<FString>& SearchRoots, THashSet<FString>& Visited, uint64& Hash);

        void GatherIncludeHashes(const FString& Source, FStringView Dir, const TVector<FString>& SearchRoots, THashSet<FString>& Visited, uint64& Hash)
        {
            // Walk lines; cheap enough for shader sizes and keeps us off a regex dep.
            size_t Pos = 0;
            while (Pos < Source.size())
            {
                size_t LineEnd = Source.find('\n', Pos);
                if (LineEnd == FString::npos) LineEnd = Source.size();
                FStringView Line(Source.data() + Pos, LineEnd - Pos);
                Pos = LineEnd + 1;

                // Skip leading whitespace.
                size_t i = 0;
                while (i < Line.size() && (Line[i] == ' ' || Line[i] == '\t')) ++i;
                if (i >= Line.size()) continue;

                FStringView Trim = Line.substr(i);

                if (Trim.size() > 7 && Trim.substr(0, 7) == "import ")
                {
                    // The import statement form, a keyword followed by a dotted module name.
                    size_t j = 7;
                    while (j < Trim.size() && (Trim[j] == ' ' || Trim[j] == '\t')) ++j;
                    size_t Start = j;
                    while (j < Trim.size() && Trim[j] != ';' && Trim[j] != ' ' && Trim[j] != '\t' && Trim[j] != '\r') ++j;
                    if (j > Start)
                    {
                        FStringView Name = Trim.substr(Start, j - Start);
                        FString Resolved = ResolveInclude(Name, true, Dir, SearchRoots);
                        if (!Resolved.empty())
                        {
                            GatherSourceHash(Resolved, SearchRoots, Visited, Hash);
                        }
                    }
                }
                else if (Trim.size() > 9 && Trim.substr(0, 8) == "#include")
                {
                    size_t Quote = Trim.find('"', 8);
                    if (Quote != FString::npos)
                    {
                        size_t Close = Trim.find('"', Quote + 1);
                        if (Close != FString::npos)
                        {
                            FStringView Name = Trim.substr(Quote + 1, Close - Quote - 1);
                            FString Resolved = ResolveInclude(Name, false, Dir, SearchRoots);
                            if (!Resolved.empty())
                            {
                                GatherSourceHash(Resolved, SearchRoots, Visited, Hash);
                            }
                        }
                    }
                }
            }

        }

        // False when the file could not be read, and the caller turns that into do-not-cache.
        bool GatherSourceHash(FStringView VirtualPath, const TVector<FString>& SearchRoots, THashSet<FString>& Visited, uint64& Hash)
        {
            FString PathStr(VirtualPath.data(), VirtualPath.size());
            if (Visited.find(PathStr) != Visited.end())
            {
                return true;
            }
            Visited.insert(PathStr);

            FString Source;
            if (!VFS::ReadFile(Source, VirtualPath))
            {
                return false;
            }

            const uint64 FileHash = Hash::GetHash64(Source);
            Hash ^= FileHash + 0x9e3779b97f4a7c15ULL + (Hash << 6) + (Hash >> 2);

            // An unresolved include is not fatal, since Slang may still find it and only the root matters.
            GatherIncludeHashes(Source, ParentDir(VirtualPath), SearchRoots, Visited, Hash);
            return true;
        }
    }

    uint64 ComputeSourceSetHash(FStringView ShaderVirtualPath, const TVector<FString>& Defines, const TVector<FString>& SearchRoots, FStringView EntryPoint)
    {
        uint64 Hash = (uint64)kShaderCacheVersion;
        const FString DefineBlob = JoinSortedDefines(Defines);
        Hash ^= Hash::GetHash64(DefineBlob) + 0x9e3779b97f4a7c15ULL + (Hash << 6) + (Hash >> 2);
        Hash ^= Hash::GetHash64(EntryPoint) + 0x9e3779b97f4a7c15ULL + (Hash << 6) + (Hash >> 2);

        THashSet<FString> Visited;
        if (!GatherSourceHash(ShaderVirtualPath, SearchRoots, Visited, Hash))
        {
            return 0;
        }

        // 0 is reserved as "skip source-hash check", never return it.
        if (Hash == 0)
        {
            Hash = 1;
        }
        return Hash;
    }

    uint64 ComputeRawSourceHash(FStringView Source, const TVector<FString>& Defines, const TVector<FString>& SearchRoots, FStringView TemplateVirtualPath, FStringView EntryPoint)
    {
        // Without the template a relative include cannot resolve, so an edited header would not invalidate.
        if (TemplateVirtualPath.empty())
        {
            return 0;
        }

        uint64 Hash = (uint64)kShaderCacheVersion;
        const FString DefineBlob = JoinSortedDefines(Defines);
        Hash ^= Hash::GetHash64(DefineBlob) + 0x9e3779b97f4a7c15ULL + (Hash << 6) + (Hash >> 2);

        const FString SourceText(Source.data(), Source.size());
        Hash ^= Hash::GetHash64(SourceText) + 0x9e3779b97f4a7c15ULL + (Hash << 6) + (Hash >> 2);
        Hash ^= Hash::GetHash64(EntryPoint) + 0x9e3779b97f4a7c15ULL + (Hash << 6) + (Hash >> 2);

        THashSet<FString> Visited;
        GatherIncludeHashes(SourceText, ParentDir(TemplateVirtualPath), SearchRoots, Visited, Hash);

        // 0 is reserved for do-not-cache, never return it.
        return Hash == 0 ? 1 : Hash;
    }

    static FString CachePathForKey(uint64 KeyHash)
    {
        char HexBuf[32];
        snprintf(HexBuf, sizeof(HexBuf), "%016llx", (unsigned long long)KeyHash);

        FString Out = kCacheDirectory;
        Out += "/raw_";
        Out += HexBuf;
        Out += ".lsc";
        return Out;
    }

    bool TryLoadRaw(uint64 KeyHash, FShaderHeader& OutHeader)
    {
        if (KeyHash == 0)
        {
            return false;
        }
        return TryLoadByCachePath(CachePathForKey(KeyHash), KeyHash, OutHeader);
    }

    bool SaveRaw(uint64 KeyHash, const FShaderHeader& Header)
    {
        LUMINA_MEMORY_SCOPE("Shaders");
        if (KeyHash == 0)
        {
            return false;
        }

        VFS::CreateDir(kCacheDirectory);

        TVector<uint8> Bytes;
        FMemoryWriter Writer(Bytes);

        uint32 Magic = CACHE_MAGIC;
        uint32 Version = kShaderCacheVersion;
        uint64 StoredHash = KeyHash;
        Writer << Magic;
        Writer << Version;
        Writer << StoredHash;

        SerializeHeader(Writer, const_cast<FShaderHeader&>(Header));

        return VFS::AtomicWriteFile(CachePathForKey(KeyHash), TSpan<const uint8>(Bytes.data(), Bytes.size()));
    }

    FString CachePathFor(FStringView ShaderVirtualPath, const TVector<FString>& Defines, FStringView EntryPoint)
    {
        FString Key(ShaderVirtualPath.data(), ShaderVirtualPath.size());
        Key += '|';
        Key.append(EntryPoint.data(), EntryPoint.size());
        Key += '|';
        Key += JoinSortedDefines(Defines);
        const uint64 KeyHash = Hash::GetHash64(Key);

        char HexBuf[32];
        snprintf(HexBuf, sizeof(HexBuf), "%016llx", (unsigned long long)KeyHash);

        FString Out = kCacheDirectory;
        Out += '/';
        Out += HexBuf;
        Out += ".lsc";
        return Out;
    }

    bool TryLoadByCachePath(FStringView CacheVirtualPath, uint64 SourceHash, FShaderHeader& OutHeader)
    {
        LUMINA_MEMORY_SCOPE("Shaders");
        TVector<uint8> Bytes;
        if (!VFS::ReadFile(Bytes, CacheVirtualPath))
        {
            return false;
        }
        if (Bytes.size() < sizeof(uint32) * 2 + sizeof(uint64))
        {
            return false;
        }

        FMemoryReader Reader(Bytes);

        uint32 Magic = 0;
        uint32 Version = 0;
        uint64 StoredHash = 0;
        Reader << Magic;
        Reader << Version;
        Reader << StoredHash;

        if (Magic != CACHE_MAGIC || Version != kShaderCacheVersion)
        {
            return false;
        }
        if (SourceHash != 0 && SourceHash != StoredHash)
        {
            return false;
        }

        SerializeHeader(Reader, OutHeader);
        return !Reader.HasError();
    }

    bool TryLoad(FStringView ShaderVirtualPath, const TVector<FString>& Defines, FStringView EntryPoint, uint64 SourceHash, FShaderHeader& OutHeader)
    {
        const FString CachePath = CachePathFor(ShaderVirtualPath, Defines, EntryPoint);
        return TryLoadByCachePath(CachePath, SourceHash, OutHeader);
    }

    bool Save(FStringView ShaderVirtualPath, const TVector<FString>& Defines, FStringView EntryPoint, uint64 SourceHash, const FShaderHeader& Header)
    {
        LUMINA_MEMORY_SCOPE("Shaders");
        if (SourceHash == 0)
        {
            return false;
        }

        // Make sure the cache directory exists; native FS WriteFile won't mkdir.
        VFS::CreateDir(kCacheDirectory);

        TVector<uint8> Bytes;
        FMemoryWriter Writer(Bytes);

        uint32 Magic = CACHE_MAGIC;
        uint32 Version = kShaderCacheVersion;
        uint64 StoredHash = SourceHash;
        Writer << Magic;
        Writer << Version;
        Writer << StoredHash;

        SerializeHeader(Writer, const_cast<FShaderHeader&>(Header));

        const FString CachePath = CachePathFor(ShaderVirtualPath, Defines, EntryPoint);
        return VFS::AtomicWriteFile(CachePath, TSpan<const uint8>(Bytes.data(), Bytes.size()));
    }
}
