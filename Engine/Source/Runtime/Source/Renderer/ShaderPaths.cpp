#include "RuntimePCH.h"
#include "ShaderPaths.h"

#include "ShaderCache.h"
#include "ShaderCompiler.h"
#include "ShaderLibrary.h"
#include "Containers/Algorithm.h"
#include "Core/Math/Hash/Hash.h"
#include "Core/Plugin/Plugin.h"
#include "Core/Plugin/PluginManager.h"
#include "Core/Threading/Thread.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"
#include "Paths/Paths.h"

namespace Lumina::Shaders
{
    namespace
    {
        constexpr const char* kEngineRoot  = "/Engine/Resources/Shaders";
        constexpr const char* kProjectRoot = "/Game/Shaders";
        constexpr const char* kPluginSubdir = "/Shaders";

        FMutex              GMutex;
        TVector<FString>    GExtraRoots;         // module-registered, in registration order
        THashSet<FString>   GEnumeratedRoots;    // roots PrecompileNewRoots has already walked

        // A plugin without shaders must not put a dead path in Slang's search list.
        void AppendRoot(TVector<FString>& Out, FString Root)
        {
            if (Root.IsEmpty() || !VFS::Exists(Root))
            {
                return;
            }
            for (const FString& Existing : Out)
            {
                if (Existing == Root)
                {
                    return;
                }
            }
            Out.Emplace(Move(Root));
        }
    }

    void GetSearchRoots(TVector<FString>& OutRoots)
    {
        OutRoots.Reset();
        OutRoots.Reserve(8);

        // Engine first, locked against accidental shadowing by a plugin or game file of the same name.
        AppendRoot(OutRoots, kEngineRoot);

        for (const FPlugin* Plugin : FPluginManager::Get().GetAllPlugins())
        {
            if (!Plugin->IsEnabled() || !Plugin->IsContentMounted())
            {
                continue;
            }
            AppendRoot(OutRoots, Plugin->GetMountAlias() + kPluginSubdir);
        }

        AppendRoot(OutRoots, kProjectRoot);

        FScopeLock Lock(GMutex);
        for (const FString& Root : GExtraRoots)
        {
            AppendRoot(OutRoots, Root);
        }
    }

    void RegisterSearchRoot(FStringView VirtualRoot)
    {
        if (VirtualRoot.IsEmpty())
        {
            return;
        }

        FString Root(Paths::TrimTrailingSeparators(VirtualRoot));

        FScopeLock Lock(GMutex);
        for (const FString& Existing : GExtraRoots)
        {
            if (Existing == Root)
            {
                return;
            }
        }
        GExtraRoots.Emplace(Move(Root));
    }

    void UnregisterSearchRoot(FStringView VirtualRoot)
    {
        const FString Root(Paths::TrimTrailingSeparators(VirtualRoot));

        FScopeLock Lock(GMutex);
        GExtraRoots.RemoveSingle(Root);
    }

    FString Resolve(FStringView NameOrPath)
    {
        if (NameOrPath.IsEmpty())
        {
            return {};
        }

        FString Name(NameOrPath.GetData(), NameOrPath.Len());

        // Already rooted, which is how a shader whose name collides with an earlier root is reached.
        if (Name[0] == '/')
        {
            return VFS::Exists(Name) ? Name : FString();
        }

        TVector<FString> Roots;
        GetSearchRoots(Roots);

        for (const FString& Root : Roots)
        {
            FString Candidate = Root;
            Candidate += '/';
            Candidate += Name;
            if (VFS::Exists(Candidate))
            {
                return Candidate;
            }
        }

        return {};
    }

    uint32 PrecompileNewRoots()
    {
        if (GShaderCompiler == nullptr)
        {
            return 0;
        }

        TVector<FString> Roots;
        GetSearchRoots(Roots);

        TVector<FString> ShaderPaths;
        for (const FString& Root : Roots)
        {
            {
                FScopeLock Lock(GMutex);
                if (!GEnumeratedRoots.TryAdd(Root))
                {
                    continue;
                }
            }

            // Includes and tokenized templates are not standalone modules and must not compile alone.
            VFS::DirectoryIterator(Root, [&ShaderPaths](const VFS::FFileInfo& Info)
            {
                if (Info.GetExt() == ".slang")
                {
                    ShaderPaths.Emplace(*Info.VirtualPath);
                }
            });
        }

        if (ShaderPaths.IsEmpty())
        {
            return 0;
        }

        TVector<FShaderCompileOptions> Options(ShaderPaths.Num());
        for (FShaderCompileOptions& Option : Options)
        {
            Option.bGenerateReflectionData = false;
        }

        GShaderCompiler->CompileShaderPaths(ShaderPaths, Options, [](const FShaderHeader& Header)
        {
            FShaderLibrary::Commit(Header);
        });

        return (uint32)ShaderPaths.Num();
    }

    uint64 HashSourceDirectories(TSpan<const FStringView> Directories)
    {
        struct FEntry
        {
            FString Path;
            uint64  ContentHash;
        };
        TVector<FEntry> Files;

        for (FStringView Directory : Directories)
        {
            VFS::RecursiveDirectoryIterator(Directory, [&Files](const VFS::FFileInfo& Info)
            {
                if (Info.IsDirectory() || (Info.GetExt() != ".slang" && Info.GetExt() != ".h"))
                {
                    return;
                }
                FString Source;
                if (VFS::ReadFile(Source, *Info.VirtualPath))
                {
                    // Checkout line endings differ per machine and must not change the hash.
                    const char* const Kept = Algo::Remove(Source, '\r');
                    Source.LeftInline(static_cast<int32>(Kept - Source.GetData()));
                    Files.Emplace(FString(*Info.VirtualPath), Hash::XXHash::GetHash64(Source));
                }
            });
        }

        Algo::Sort(Files, [](const FEntry& A, const FEntry& B) { return A.Path < B.Path; });

        size_t Result = Files.Num();
        Hash::HashCombine(Result, (size_t)FShaderCache::kShaderCacheVersion);
        for (const FEntry& File : Files)
        {
            Hash::HashCombine(Result, Hash::GetHash64(File.Path));
            Hash::HashCombine(Result, (size_t)File.ContentHash);
        }

        // 0 is the serialized never-compiled sentinel, so a real hash must never collide with it.
        return Result != 0 ? (uint64)Result : 1ull;
    }
}
