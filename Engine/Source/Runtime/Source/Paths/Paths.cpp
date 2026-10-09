#include "RuntimePCH.h"
#include "Paths.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "Core/Assertions/Assert.h"
#include "Platform/Process/PlatformProcess.h"
#include "Log/Log.h"

namespace Lumina::Paths
{
    static THashMap<FName, FString> CachedDirectories;

    // Computed from the exe path alone, since the log and crash handler ask before paths are initialized.
    const FString& GetGameDataDirectory()
    {
        static const FString Directory = []
        {
            FString ExePath = Platform::GetCurrentProcessPath();
            Normalize(ExePath);

            const size_t Slash = ExePath.find_last_of('/');
            const FString ExeDir = Slash == FString::npos ? FString() : ExePath.substr(0, Slash);
            FString Stem = Slash == FString::npos ? ExePath : ExePath.substr(Slash + 1);
            if (const size_t Dot = Stem.find_last_of('.'); Dot != FString::npos)
            {
                Stem.resize(Dot);
            }

            const FString DataDir = ExeDir + "/" + Stem + "_Data";
            return Filesystem::Exists(DataDir) ? DataDir : ExeDir;
        }();
        return Directory;
    }

    namespace
    {
        const char* EngineResourceDirectoryName     = "EngineResourceDirectory";
        const char* EngineFontDirectoryName         = "EngineFontDirectory";
        const char* EngineContentDirectoryName      = "EngineContentDirectory";
        const char* EngineShadersDirectoryName      = "EngineShadersDirectory";
        const char* EngineConfigDirectoryName       = "EngineConfigDirectory";
        const char* EngineInstallDirectoryName      = "EngineInstallDirectory";
        const char* EngineDirectoryName             = "EngineDirectory";

        bool bInstalledBuild = false;
    }

    void InitializePaths()
    {
        FString LuminaDir = Platform::GetEnvVariable("LUMINA_DIR");
        Normalize(LuminaDir);

        FString ExePath = Platform::GetCurrentProcessPath();
        Normalize(ExePath);

        // The exe lives two directories below the root, and without this every resource path is malformed.
        const FString Candidate = Parent(Parent(Parent(ExePath, true), true), true);

        // An installed build must not run against whatever source tree LUMINA_DIR names on this machine.
        bInstalledBuild = !Candidate.empty() && Filesystem::Exists(Candidate + "/" + InstalledBuildMarker);
        if (bInstalledBuild)
        {
            LuminaDir = Candidate;
        }
        else if (LuminaDir.empty() || !Filesystem::Exists(LuminaDir + "/Engine/Resources"))
        {
            if (!Candidate.empty() && Filesystem::Exists(Candidate + "/Engine/Resources"))
            {
                LOG_DISPLAY("LUMINA_DIR unset or invalid; using executable-relative engine root: {}", Candidate.c_str());
                LuminaDir = Candidate;
            }
            else
            {
                LOG_ERROR("Could not resolve engine root. LUMINA_DIR='{}', exe-relative candidate='{}'. "
                          "Run the engine's Setup.bat. Resources (fonts, shaders) will fail to load.",
                          LuminaDir.c_str(), Candidate.c_str());
            }
        }

        CachedDirectories[EngineInstallDirectoryName]   = LuminaDir;
        CachedDirectories[EngineDirectoryName]          = LuminaDir + "/Engine";
        CachedDirectories[EngineConfigDirectoryName]    = GetEngineDirectory() + "/Config";
        CachedDirectories[EngineResourceDirectoryName]  = GetEngineDirectory() + "/Resources";
        CachedDirectories[EngineFontDirectoryName]      = GetEngineResourceDirectory() + "/Fonts";
        CachedDirectories[EngineContentDirectoryName]   = GetEngineResourceDirectory() + "/Content";
        CachedDirectories[EngineShadersDirectoryName]   = GetEngineResourceDirectory() + "/Shaders";
        
    }

    FString GetEngineDirectory()
    {
        return CachedDirectories[EngineDirectoryName];
    }

    FString GetExtension(const FString& InPath)
    {
        size_t Dot = InPath.find_last_of('.');
        if (Dot != FString::npos && Dot + 1 < InPath.length())
        {
            return InPath.substr(Dot);
        }

        return {};
    }

    bool IsDirectory(const FString& Path)
    {
        return Filesystem::IsDirectory(Path);
    }

    bool Exists(FStringView Filename)
    {
        return Filesystem::Exists(Filename);
    }

    FString GetVirtualPathPrefix(const FString& VirtualPath)
    {
        size_t Pos = VirtualPath.find("://");
        if (Pos != FString::npos)
        {
            return VirtualPath.substr(0, Pos + 3);
        }

        return VirtualPath;
    }

    bool CreateDirectories(FStringView Path)
    {
        return Filesystem::MakeDirectoryTree(Path);
    }

    bool IsUnderDirectory(const FString& ParentDirectory, const FString& Directory)
    {
        if (Directory.length() < ParentDirectory.length())
        {
            return false;
        }

        if (!EqualsIgnoreCase(FStringView(Directory.data(), ParentDirectory.length()), FStringView(ParentDirectory.data(), ParentDirectory.length())))
        {
            return false;
        }

        if (Directory.length() > ParentDirectory.length())
        {
            char nextChar = Directory[ParentDirectory.length()];
            if (nextChar != '/' && nextChar != '\\')
            {
                return false;
            }
        }

        return true;
    }
    
    FString MakeRelativeTo(const FString& Path, const FString& BasePath)
    {
        FString NormalizedPath = Path;
        FString NormalizedBase = BasePath;
    
        Algo::Replace(NormalizedPath, '\\', '/');
        Algo::Replace(NormalizedBase, '\\', '/');
    
        if (!NormalizedBase.empty() && NormalizedBase.back() != '/')
        {
            NormalizedBase.push_back('/');
        }
    
        if (NormalizedPath.find(NormalizedBase) != 0)
        {
            return Path;
        }
    
        return NormalizedPath.substr(NormalizedBase.size());
    }

    void ReplaceFilename(FString& Path, const FString& NewFilename)
    {
        size_t LastSlash = Path.find_last_of("/\\");

        if (LastSlash != FString::npos)
        {
            Path = Path.substr(0, LastSlash + 1) + NewFilename;
            return;
        }

        Path = NewFilename;
    }

    const FString& GetEngineResourceDirectory()
    {
        return CachedDirectories[EngineResourceDirectoryName];
    }

    const FString& GetEngineFontDirectory()
    {
        return CachedDirectories[EngineFontDirectoryName];
    }

    const FString& GetEngineContentDirectory()
    {
        return CachedDirectories[EngineContentDirectoryName];
    }

    const FString& GetEngineConfigDirectory()
    {
        return CachedDirectories[EngineConfigDirectoryName];
    }

    const FString& GetEngineShadersDirectory()
    {
        return CachedDirectories[EngineShadersDirectoryName];
    }

    FString GetUserDataDirectory()
    {
        #if defined(_WIN32)
        FString Directory = Platform::GetEnvVariable("LOCALAPPDATA");
        #else
        FString Directory = Platform::GetEnvVariable("XDG_CONFIG_HOME");
        if (Directory.empty())
        {
            Directory = Platform::GetEnvVariable("HOME") + "/.config";
        }
        #endif
        Normalize(Directory);
        return Directory;
    }

    FString Parent(FStringView Path, bool bRemoveTrailingSlash)
    {
        auto data = Path.data();
        auto len = Path.size();

        size_t i = len;
        while (i > 0)
        {
            char c = data[i - 1];
            if (c == '/' || c == '\\')
            {
                break;
            }
            --i;
        }

        if (i == 0)
        {
            return FString();
        }

        if (bRemoveTrailingSlash && i > 1)
        {
            size_t j = i - 1;
            while (j > 0)
            {
                char c = data[j - 1];
                if (c != '/' && c != '\\')
                {
                    break;
                }
                --j;
            }
            i = j;
        }

        return FString(data, i);
    }

    const FString& GetEngineInstallDirectory()
    {
        return CachedDirectories[EngineInstallDirectoryName];
    }

    bool IsInstalledBuild()
    {
        return bInstalledBuild;
    }

    namespace
    {
        // Without collapsing runs, repeated joins accumulate slashes and the path grows forever.
        template<typename StringT>
        void NormalizeInPlace(StringT& Path)
        {
            // 1) backslashes â†’ forward slashes
            for (auto& c : Path)
            {
                if (c == '\\') c = '/';
            }

            // 2) collapse runs of '/' to a single '/'.
            size_t Write = 0;
            bool PrevSlash = false;
            for (size_t Read = 0; Read < Path.size(); ++Read)
            {
                const char c = Path[Read];
                if (c == '/')
                {
                    if (PrevSlash) continue;
                    PrevSlash = true;
                }
                else
                {
                    PrevSlash = false;
                }
                Path[Write++] = c;
            }
            Path.resize(Write);
        }
    }

    void Normalize(FString& Path)
    {
        NormalizeInPlace(Path);
    }

    void Normalize(FFixedString& Path)
    {
        NormalizeInPlace(Path);
    }

    FFixedString Normalize(FStringView Path)
    {
        FFixedString RetVal = { Path.begin(), Path.end() };
        NormalizeInPlace(RetVal);
        return RetVal;
    }

    bool PathsEqual(FStringView A, FStringView B)
    {
        size_t lenA = A.size();
        size_t lenB = B.size();
        if (lenA != lenB)
        {
            return false;
        }

        for (size_t i = 0; i < lenA; ++i)
        {
            char a = A[i];
            char b = B[i];

            if ((a == '/' || a == '\\') && (b == '/' || b == '\\'))
            {
                continue;
            }


            if (std::tolower(static_cast<unsigned char>(a)) != std::tolower(static_cast<unsigned char>(b)))
            {
                return false;
            }
        }

        return true;
    }
    
    FFixedString MakeGameApplicationName()
    {
        FFixedString Result = "Lumina-";
        Result.append(LUMINA_CONFIGURATION_NAME).append(LUMINA_EXECUTABLE_EXT_NAME);
        return Result;
    }

    FFixedString MakeServerApplicationName()
    {
        FFixedString Result = "Lumina-Server-";
        Result.append(LUMINA_CONFIGURATION_NAME).append(LUMINA_EXECUTABLE_EXT_NAME);
        return Result;
    }

    FFixedString MakeGameModuleFileName(FStringView ModuleName)
    {
        FFixedString Result = LUMINA_SHAREDLIB_PREFIX_NAME;
        Result.append(ModuleName.data(), ModuleName.size());
        Result.append("-").append(LUMINA_CONFIGURATION_NAME).append(LUMINA_SHAREDLIB_EXT_NAME);
        return Result;
    }

    FFixedString MakeModuleFileName(FStringView ModuleName)
    {
        FFixedString Result = LUMINA_SHAREDLIB_PREFIX_NAME;
        Result.append(ModuleName.data(), ModuleName.size());
        Result.append(LUMINA_BINARY_SUFFIX).append(LUMINA_SHAREDLIB_EXT_NAME);
        return Result;
    }

}
