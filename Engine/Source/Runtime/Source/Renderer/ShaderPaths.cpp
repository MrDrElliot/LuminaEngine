#include "RuntimePCH.h"
#include "ShaderPaths.h"

#include "ShaderCompiler.h"
#include "ShaderLibrary.h"
#include "Core/Plugin/Plugin.h"
#include "Core/Plugin/PluginManager.h"
#include "Core/Threading/Thread.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"

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
            if (Root.empty() || !VFS::Exists(Root))
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
            Out.emplace_back(Move(Root));
        }
    }

    void GetSearchRoots(TVector<FString>& OutRoots)
    {
        OutRoots.clear();
        OutRoots.reserve(8);

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
        if (VirtualRoot.empty())
        {
            return;
        }

        FString Root(VirtualRoot.data(), VirtualRoot.size());
        while (!Root.empty() && Root.back() == '/')
        {
            Root.pop_back();
        }

        FScopeLock Lock(GMutex);
        for (const FString& Existing : GExtraRoots)
        {
            if (Existing == Root)
            {
                return;
            }
        }
        GExtraRoots.emplace_back(Move(Root));
    }

    void UnregisterSearchRoot(FStringView VirtualRoot)
    {
        FString Root(VirtualRoot.data(), VirtualRoot.size());
        while (!Root.empty() && Root.back() == '/')
        {
            Root.pop_back();
        }

        FScopeLock Lock(GMutex);
        for (auto It = GExtraRoots.begin(); It != GExtraRoots.end(); ++It)
        {
            if (*It == Root)
            {
                GExtraRoots.erase(It);
                return;
            }
        }
    }

    FString Resolve(FStringView NameOrPath)
    {
        if (NameOrPath.empty())
        {
            return {};
        }

        FString Name(NameOrPath.data(), NameOrPath.size());

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
                if (!GEnumeratedRoots.insert(Root).second)
                {
                    continue;
                }
            }

            // Includes and tokenized templates are not standalone modules and must not compile alone.
            VFS::DirectoryIterator(Root, [&ShaderPaths](const VFS::FFileInfo& Info)
            {
                if (Info.GetExt() == ".slang")
                {
                    ShaderPaths.emplace_back(Info.VirtualPath.c_str());
                }
            });
        }

        if (ShaderPaths.empty())
        {
            return 0;
        }

        TVector<FShaderCompileOptions> Options(ShaderPaths.size());
        for (FShaderCompileOptions& Option : Options)
        {
            Option.bGenerateReflectionData = false;
        }

        GShaderCompiler->CompileShaderPaths(ShaderPaths, Options, [](const FShaderHeader& Header)
        {
            FShaderLibrary::Commit(Header);
        });

        return (uint32)ShaderPaths.size();
    }
}
