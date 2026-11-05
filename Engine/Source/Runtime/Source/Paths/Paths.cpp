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

            const int32 Slash = ExePath.FindLastOf('/');
            const FString ExeDir = Slash == Constants::kIndexNone ? FString() : ExePath.Mid(0, Slash);
            FString Stem = Slash == Constants::kIndexNone ? ExePath : ExePath.Mid(Slash + 1);
            if (const int32 Dot = Stem.FindLastOf('.'); Dot != Constants::kIndexNone)
            {
                Stem.SetLen(Dot);
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
        FString ExePath = Platform::GetCurrentProcessPath();
        Normalize(ExePath);

        // The exe lives in <root>/Binaries/<Platform>, so every checkout runs against its own engine tree.
        const FString LuminaDir = Parent(Parent(Parent(ExePath, true), true), true);

        bInstalledBuild = !LuminaDir.IsEmpty() && Filesystem::Exists(LuminaDir + "/" + InstalledBuildMarker);
        // A packaged game has no engine tree beside it, since its engine resources come from the pak.
        const FString& DataDir = GetGameDataDirectory();
        const bool bPackaged = DataDir.Len() > 5 && DataDir.EndsWith("_Data");
        if (!bPackaged && (LuminaDir.IsEmpty() || !Filesystem::Exists(LuminaDir + "/Engine/Resources")))
        {
            LOG_ERROR("Could not resolve the engine root from '{}'. Resources (fonts, shaders) will fail to load.", *ExePath);
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
        int32 Dot = InPath.FindLastOf('.');
        if (Dot != Constants::kIndexNone && Dot + 1 < InPath.Len())
        {
            return InPath.Mid(Dot);
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
        int32 Pos = VirtualPath.Find("://");
        if (Pos != Constants::kIndexNone)
        {
            return VirtualPath.Mid(0, Pos + 3);
        }

        return VirtualPath;
    }

    bool CreateDirectories(FStringView Path)
    {
        return Filesystem::MakeDirectoryTree(Path);
    }

    bool IsUnderDirectory(const FString& ParentDirectory, const FString& Directory)
    {
        if (Directory.Len() < ParentDirectory.Len())
        {
            return false;
        }

        if (!EqualsIgnoreCase(FStringView(Directory.GetData(), ParentDirectory.Len()), FStringView(ParentDirectory.GetData(), ParentDirectory.Len())))
        {
            return false;
        }

        if (Directory.Len() > ParentDirectory.Len())
        {
            char nextChar = Directory[ParentDirectory.Len()];
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
    
        if (!NormalizedBase.IsEmpty() && NormalizedBase.Last() != '/')
        {
            NormalizedBase.AppendChar('/');
        }
    
        if (NormalizedPath.Find(NormalizedBase) != 0)
        {
            return Path;
        }
    
        return NormalizedPath.Mid(NormalizedBase.Len());
    }

    void ReplaceFilename(FString& Path, const FString& NewFilename)
    {
        int32 LastSlash = Path.FindLastOf("/\\");

        if (LastSlash != Constants::kIndexNone)
        {
            Path = Path.Mid(0, LastSlash + 1) + NewFilename;
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
        if (Directory.IsEmpty())
        {
            Directory = Platform::GetEnvVariable("HOME") + "/.config";
        }
        #endif
        Normalize(Directory);
        return Directory;
    }

    FString Parent(FStringView Path, bool bRemoveTrailingSlash)
    {
        auto data = Path.GetData();
        auto len = Path.Len();

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

        return FString(data, IntCastChecked<int32>(i));
    }

    const FString& GetEngineInstallDirectory()
    {
        return CachedDirectories[EngineInstallDirectoryName];
    }

    bool IsInstalledBuild()
    {
        return bInstalledBuild;
    }

    FString GetBuildToolScript()
    {
        #if defined(LE_PLATFORM_WINDOWS)
        FString Script = GetEngineInstallDirectory() + "/LuminaBuild.bat";
        #else
        FString Script = GetEngineInstallDirectory() + "/LuminaBuild.sh";
        #endif
        return !bInstalledBuild && Exists(Script) ? Script : FString();
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
            int32 Write = 0;
            bool PrevSlash = false;
            for (int32 Read = 0; Read < Path.Len(); ++Read)
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
            Path.SetLen(Write);
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
        int32 lenA = A.Len();
        int32 lenB = B.Len();
        if (lenA != lenB)
        {
            return false;
        }

        for (int32 i = 0; i < lenA; ++i)
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
        Result.Append(LUMINA_CONFIGURATION_NAME).Append(LUMINA_EXECUTABLE_EXT_NAME);
        return Result;
    }

    FFixedString MakeServerApplicationName()
    {
        FFixedString Result = "Lumina-Server-";
        Result.Append(LUMINA_CONFIGURATION_NAME).Append(LUMINA_EXECUTABLE_EXT_NAME);
        return Result;
    }

    FFixedString MakeGameModuleFileName(FStringView ModuleName)
    {
        FFixedString Result = LUMINA_SHAREDLIB_PREFIX_NAME;
        Result.Append(ModuleName.GetData(), ModuleName.Len());
        Result.Append("-").Append(LUMINA_CONFIGURATION_NAME).Append(LUMINA_SHAREDLIB_EXT_NAME);
        return Result;
    }

    FFixedString MakeModuleFileName(FStringView ModuleName)
    {
        FFixedString Result = LUMINA_SHAREDLIB_PREFIX_NAME;
        Result.Append(ModuleName.GetData(), ModuleName.Len());
        Result.Append(LUMINA_BINARY_SUFFIX).Append(LUMINA_SHAREDLIB_EXT_NAME);
        return Result;
    }

}
