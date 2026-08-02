#include "RuntimePCH.h"
#include "ShaderCache.h"
#include "RenderResource.h"
#include "Core/Math/Hash/Hash.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"

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
            eastl::sort(Sorted.begin(), Sorted.end());
            FString Joined;
            for (const FString& D : Sorted)
            {
                Joined += D;
                Joined += '\n';
            }
            return Joined;
        }

        FString ResolveInclude(FStringView Token, bool bIsImport, FStringView IncludingDir, FStringView ShaderRoot)
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

            // Try sibling-of-includer first, then shader root.
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

            FString Root(ShaderRoot.data(), ShaderRoot.size());
            if (!Root.empty() && Root.back() != '/')
            {
                Root += '/';
            }
            Root += Candidate;
            if (VFS::Exists(Root))
            {
                return Root;
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

        void GatherSourceHash(FStringView VirtualPath, FStringView ShaderRoot, THashSet<FString>& Visited, uint64& Hash)
        {
            FString PathStr(VirtualPath.data(), VirtualPath.size());
            if (Visited.find(PathStr) != Visited.end())
            {
                return;
            }
            Visited.insert(PathStr);

            FString Source;
            if (!VFS::ReadFile(Source, VirtualPath))
            {
                return;
            }

            const uint64 FileHash = Hash::GetHash64(Source);
            Hash ^= FileHash + 0x9e3779b97f4a7c15ULL + (Hash << 6) + (Hash >> 2);

            FStringView Dir = ParentDir(VirtualPath);

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
                    // Form: import Foo.Bar.Baz ;
                    size_t j = 7;
                    while (j < Trim.size() && (Trim[j] == ' ' || Trim[j] == '\t')) ++j;
                    size_t Start = j;
                    while (j < Trim.size() && Trim[j] != ';' && Trim[j] != ' ' && Trim[j] != '\t' && Trim[j] != '\r') ++j;
                    if (j > Start)
                    {
                        FStringView Name = Trim.substr(Start, j - Start);
                        FString Resolved = ResolveInclude(Name, true, Dir, ShaderRoot);
                        if (!Resolved.empty())
                        {
                            GatherSourceHash(Resolved, ShaderRoot, Visited, Hash);
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
                            FString Resolved = ResolveInclude(Name, false, Dir, ShaderRoot);
                            if (!Resolved.empty())
                            {
                                GatherSourceHash(Resolved, ShaderRoot, Visited, Hash);
                            }
                        }
                    }
                }
            }
        }
    }

    uint64 ComputeSourceSetHash(FStringView ShaderVirtualPath, const TVector<FString>& Defines)
    {
        uint64 Hash = (uint64)SHADER_CACHE_VERSION;
        const FString DefineBlob = JoinSortedDefines(Defines);
        Hash ^= Hash::GetHash64(DefineBlob) + 0x9e3779b97f4a7c15ULL + (Hash << 6) + (Hash >> 2);

        constexpr FStringView ShaderRoot = "/Engine/Resources/Shaders";
        THashSet<FString> Visited;
        GatherSourceHash(ShaderVirtualPath, ShaderRoot, Visited, Hash);

        // 0 is reserved as "skip source-hash check", never return it.
        if (Hash == 0)
        {
            Hash = 1;
        }
        return Hash;
    }

    FString CachePathFor(FStringView ShaderVirtualPath, const TVector<FString>& Defines)
    {
        FString Key(ShaderVirtualPath.data(), ShaderVirtualPath.size());
        Key += '|';
        Key += JoinSortedDefines(Defines);
        const uint64 KeyHash = Hash::GetHash64(Key);

        char HexBuf[32];
        snprintf(HexBuf, sizeof(HexBuf), "%016llx", (unsigned long long)KeyHash);

        FString Out = CACHE_DIR;
        Out += '/';
        Out += HexBuf;
        Out += ".lsc";
        return Out;
    }

    bool TryLoadByCachePath(FStringView CacheVirtualPath, uint64 SourceHash, FShaderHeader& OutHeader)
    {
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

        if (Magic != CACHE_MAGIC || Version != SHADER_CACHE_VERSION)
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

    bool TryLoad(FStringView ShaderVirtualPath, const TVector<FString>& Defines, uint64 SourceHash, FShaderHeader& OutHeader)
    {
        const FString CachePath = CachePathFor(ShaderVirtualPath, Defines);
        return TryLoadByCachePath(CachePath, SourceHash, OutHeader);
    }

    bool Save(FStringView ShaderVirtualPath, const TVector<FString>& Defines, uint64 SourceHash, const FShaderHeader& Header)
    {
        if (SourceHash == 0)
        {
            return false;
        }

        // Make sure the cache directory exists; native FS WriteFile won't mkdir.
        VFS::CreateDir(CACHE_DIR);

        TVector<uint8> Bytes;
        FMemoryWriter Writer(Bytes);

        uint32 Magic = CACHE_MAGIC;
        uint32 Version = SHADER_CACHE_VERSION;
        uint64 StoredHash = SourceHash;
        Writer << Magic;
        Writer << Version;
        Writer << StoredHash;

        SerializeHeader(Writer, const_cast<FShaderHeader&>(Header));

        const FString CachePath = CachePathFor(ShaderVirtualPath, Defines);
        return VFS::AtomicWriteFile(CachePath, TSpan<const uint8>(Bytes.data(), Bytes.size()));
    }
}
