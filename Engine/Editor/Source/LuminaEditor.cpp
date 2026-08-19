#include "LuminaEditor.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include <fstream>
#include <thread>
#include <Core/Engine/Engine.h>
#include <Core/Engine/EngineMetaContext.h>
#include <Core/Plugin/PluginManager.h>
#include <Core/Progress/SlowTask.h>
#include <Memory/Memory.h>
#include <Tools/UI/DevelopmentToolUI.h>
#include "Config/Config.h"
#include "FileSystem/FileSystem.h"
#include "GUID/GUID.h"
#include "Log/Log.h"
#include "Paths/Paths.h"
#include "Platform/Process/PlatformProcess.h"
#include "UI/EditorUI.h"
#include "World/WorldManager.h"

namespace Lumina
{
    EDITOR_API FEditorEngine* GEditorEngine = nullptr;

    bool FEditorEngine::Init()
    {
        // Keep LUMINA_DIR in sync with this engine install. The editor knows the
        // authoritative root (Paths resolved it from this exe's location), so it
        // heals a missing or stale env var for everything downstream that still
        // depends on it -- shells, the IDE, and external game-project builds whose
        // the build tool hard-fails without it. Process-local set covers tools we spawn
        // this session; the persist covers future sessions. Editor-only on purpose:
        // a shipped game must never touch the player's environment.
        const FString& EngineRoot = Paths::GetEngineInstallDirectory();
        if (!EngineRoot.empty())
        {
            Platform::SetEnvVariable("LUMINA_DIR", EngineRoot);
            if (Platform::PersistUserEnvVariable("LUMINA_DIR", EngineRoot))
            {
                LOG_DISPLAY("Persisted LUMINA_DIR={} for future shells and build tools.", EngineRoot);
            }
        }

        VFS::Mount<VFS::FNativeFileSystem>("/Editor", Paths::Combine(Paths::GetEngineDirectory(), "Editor"));

        GConfig->LoadPath("/Editor/Config");

        bool bSuccess = FEngine::Init();

        // Editor + runtime settings classes are registered and /Editor is mounted by now.
        // Project settings (under /Config) load on a later pass once a project mounts it.
        GConfig->DiscoverAndLoadSettings();

        entt::locator<entt::meta_ctx>::reset(Lumina::GetEngineMetaService());

        return bSuccess;
    }

    bool FEditorEngine::Shutdown()
    {
        return FEngine::Shutdown();
    }

    CWorld* FEditorEngine::GetCurrentEditorWorld() const
    {
        return nullptr;
    }

    IDevelopmentToolUI* FEditorEngine::CreateDevelopmentTools()
    {
        return Memory::New<FEditorUI>();
    }
    
    struct FProjectTemplateContext
    {
        FString Name;
        FString NameUpper;
        FString Guid;
        FString Description;
        FString LuminaDir;        // Absolute path to engine install, fwd-slashed
        FString LuminaDirBackslash; // Same, backslashed (for .run.xml and Windows tools)
    };

    static void ReplaceProjectTokens(FString& Text, const FProjectTemplateContext& Ctx)
    {
        auto ReplaceAll = [](FString& Str, const FString& From, const FString& To)
        {
            if (From.empty())
            {
                return;
            }
            size_t Pos = 0;
            while ((Pos = Str.find(From, Pos)) != FString::npos)
            {
                Str.replace(Pos, From.size(), To);
                Pos += To.size();
            }
        };

        // Longer tokens first so $PROJECTNAME doesn't eat $PROJECTNAMEUPPER.
        ReplaceAll(Text, "$PROJECTNAMEUPPER", Ctx.NameUpper);
        ReplaceAll(Text, "$PROJECTDESCRIPTION", Ctx.Description);
        ReplaceAll(Text, "$PROJECTGUID", Ctx.Guid);
        ReplaceAll(Text, "$LUMINADIRBACKSLASH", Ctx.LuminaDirBackslash);
        ReplaceAll(Text, "$LUMINADIR", Ctx.LuminaDir);
        ReplaceAll(Text, "$PROJECTNAME", Ctx.Name);
    }

    // Tokens for the plugin template. The runtime/editor module names and their
    // uppercased API-macro forms are precomputed so the template never has to
    // compose tokens, which keeps replacement order-independent.
    struct FPluginTemplateContext
    {
        FString Name;               // e.g. "Combat"
        FString NameUpper;          // e.g. "COMBAT"
        FString Description;
        FString RuntimeModule;      // e.g. "CombatRuntime"
        FString RuntimeModuleUpper; // e.g. "COMBATRUNTIME"
        FString EditorModule;       // e.g. "CombatEditor"
        FString EditorModuleUpper;  // e.g. "COMBATEDITOR"
    };

    static void ReplacePluginTokens(FString& Text, const FPluginTemplateContext& Ctx)
    {
        auto ReplaceAll = [](FString& Str, const FString& From, const FString& To)
        {
            if (From.empty())
            {
                return;
            }
            size_t Pos = 0;
            while ((Pos = Str.find(From, Pos)) != FString::npos)
            {
                Str.replace(Pos, From.size(), To);
                Pos += To.size();
            }
        };

        // Longest / most-specific tokens first so prefixes (e.g. $RUNTIMEMODULE
        // inside $RUNTIMEMODULEUPPER, or $PLUGINNAME inside $PLUGINNAMEUPPER)
        // aren't eaten early.
        ReplaceAll(Text, "$RUNTIMEMODULEUPPER", Ctx.RuntimeModuleUpper);
        ReplaceAll(Text, "$EDITORMODULEUPPER", Ctx.EditorModuleUpper);
        ReplaceAll(Text, "$PLUGINNAMEUPPER", Ctx.NameUpper);
        ReplaceAll(Text, "$PLUGINDESCRIPTION", Ctx.Description);
        ReplaceAll(Text, "$RUNTIMEMODULE", Ctx.RuntimeModule);
        ReplaceAll(Text, "$EDITORMODULE", Ctx.EditorModule);
        ReplaceAll(Text, "$PLUGINNAME", Ctx.Name);
    }

    // Recursively copies a template tree to DestDir, running ReplaceTokens over
    // both relative paths (so $TOKEN filenames are substituted) and the contents
    // of text files. Binary files are copied verbatim. Shared by project and
    // plugin scaffolding.
    static bool CopyTemplateTree(
        const FFixedString&              TemplateDir,
        const FFixedString&              DestDir,
        const TFunction<void(FString&)>& ReplaceTokens,
        FString&                         OutError)
    {
        if (!Filesystem::MakeDirectoryTree(DestDir))
        {
            OutError = "Failed to create directory: ";
            OutError.append(DestDir.c_str(), DestDir.size());
            return false;
        }

        const size_t TemplateRootLength = TemplateDir.size();

        Filesystem::IterateDirectoryRecursive(TemplateDir, [&](const Filesystem::FDirectoryEntry& Entry)
        {
            FString RelativePathStr(Entry.FullPath.substr(TemplateRootLength + 1));

            ReplaceTokens(RelativePathStr);
            FFixedString DestPath = Paths::Combine(DestDir, RelativePathStr);

            if (Entry.IsDirectory())
            {
                Filesystem::MakeDirectoryTree(DestPath);
            }
            else
            {
                Filesystem::MakeParentDirectoryTree(DestPath);

                const FStringView SourcePath = Entry.FullPath;
                const FStringView Ext        = Entry.GetExtension();

                // Token replace only on text files; anything else copies verbatim. .cs covers both
                // the C# scripts and the .Build.cs / .Target.cs rules files, which carry
                // $PROJECTNAME in their contents and in their file names.
                const bool bIsTextFile =
                    Ext == ".h"          || Ext == ".hpp"        || Ext == ".cpp"     ||
                    Ext == ".c"          || Ext == ".inl"        || Ext == ".lua"     ||
                    Ext == ".cs"         || Ext == ".rml"        || Ext == ".rcss"    ||
                    Ext == ".json"       || Ext == ".lproject"   || Ext == ".lplugin" ||
                    Ext == ".bat"        || Ext == ".sh"         || Ext == ".py"      ||
                    Ext == ".md"         ||
                    Ext == ".txt"        || Ext == ".gitignore"  || Ext == ".cfg"     ||
                    Ext == ".yaml"       || Ext == ".yml"        || Ext == ".xml";

                if (!bIsTextFile)
                {
                    Filesystem::Copy(SourcePath, DestPath, true);
                    return;
                }

                FString FileContents;
                if (!Filesystem::ReadFile(FileContents, SourcePath))
                {
                    return;
                }

                ReplaceTokens(FileContents);

                const TSpan<const uint8> Bytes(reinterpret_cast<const uint8*>(FileContents.data()), FileContents.size());
                if (Filesystem::WriteFile(DestPath, Bytes))
                {
                    // A fresh write drops the executable bit a generated GenerateProject.sh needs.
                    Filesystem::CopyPermissions(SourcePath, DestPath);
                }
            }
        });

        return true;
    }

    // Project name must produce a valid C identifier (it becomes the module
    // name, vcxproj name, and goes into source identifiers via the API macro).
    static bool ValidateProjectName(FStringView Name, FString& OutError)
    {
        if (Name.empty())
        {
            OutError = "Project name is empty.";
            return false;
        }

        const char First = Name.front();
        if (!std::isalpha(static_cast<unsigned char>(First)) && First != '_')
        {
            OutError = "Project name must start with a letter or underscore.";
            return false;
        }

        for (char c : Name)
        {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-')
            {
                OutError = "Project name may only contain letters, digits, underscores and hyphens.";
                return false;
            }
        }

        if (Name.size() > 64)
        {
            OutError = "Project name is too long (max 64 chars).";
            return false;
        }

        return true;
    }

    bool FEditorEngine::CreateProject(FStringView NewProjectName, FStringView NewProjectPath, FFixedString& OutProjectFile, FString& OutError)
    {
        OutProjectFile.clear();
        OutError.clear();

        if (!ValidateProjectName(NewProjectName, OutError))
        {
            return false;
        }

        if (NewProjectPath.empty())
        {
            OutError = "Project location is empty.";
            return false;
        }

        const FString& EngineDir = Paths::GetEngineInstallDirectory();
        if (EngineDir.empty())
        {
            OutError = "Engine install directory is not set (LUMINA_DIR missing). Run the engine's Setup.bat.";
            return false;
        }

        const FFixedString BlankProjectPath = Paths::Combine(EngineDir, "Templates", "Blank");
        if (!Paths::Exists(BlankProjectPath))
        {
            OutError = "Blank template not found at: ";
            OutError.append(BlankProjectPath.c_str(), BlankProjectPath.size());
            return false;
        }

        const FFixedString Combined = Paths::Combine(NewProjectPath, NewProjectName);
        if (!Filesystem::Exists(NewProjectPath))
        {
            OutError = "Project location does not exist: ";
            OutError.append(NewProjectPath.data(), NewProjectPath.size());
            return false;
        }

        if (Filesystem::Exists(Combined) && !Filesystem::IsDirectoryEmpty(Combined))
        {
            OutError = "A non-empty folder already exists at: ";
            OutError.append(Combined.c_str(), Combined.size());
            return false;
        }

        FProjectTemplateContext Ctx;
        Ctx.Name.assign(NewProjectName.data(), NewProjectName.size());
        Ctx.NameUpper = Ctx.Name;
        std::transform(
            Ctx.NameUpper.begin(),
            Ctx.NameUpper.end(),
            Ctx.NameUpper.begin(),
            [](unsigned char c)
            {
                return static_cast<char>(std::toupper(c));
            });
        Ctx.Guid = FGuid::New().ToString(true, true);
        Ctx.Description = "A Lumina game project";
        Ctx.LuminaDir = EngineDir;
        std::replace(Ctx.LuminaDir.begin(), Ctx.LuminaDir.end(), '\\', '/');
        Ctx.LuminaDirBackslash = Ctx.LuminaDir;
        std::replace(Ctx.LuminaDirBackslash.begin(), Ctx.LuminaDirBackslash.end(), '/', '\\');

        if (!CopyTemplateTree(BlankProjectPath, Combined,
            [&Ctx](FString& Text) { ReplaceProjectTokens(Text, Ctx); },
            OutError))
        {
            return false;
        }

        OutProjectFile = Paths::Combine(Combined, FFixedString(Ctx.Name.c_str()).append(".lproject"));
        return true;
    }

    bool FEditorEngine::CreatePlugin(FStringView NewPluginName, FStringView Description, FFixedString& OutPluginDir, FString& OutError)
    {
        OutPluginDir.clear();
        OutError.clear();

        if (!ValidateProjectName(NewPluginName, OutError))
        {
            return false;
        }

        // Project-local plugins live next to the .lproject; we need one loaded.
        if (GetProjectName().empty())
        {
            OutError = "No project is loaded. Open a project before creating a plugin.";
            return false;
        }

        const FString& EngineDir = Paths::GetEngineInstallDirectory();
        if (EngineDir.empty())
        {
            OutError = "Engine install directory is not set (LUMINA_DIR missing). Run the engine's Setup.bat.";
            return false;
        }

        const FFixedString TemplatePath = Paths::Combine(EngineDir, "Templates", "Plugin");
        if (!Paths::Exists(TemplatePath))
        {
            OutError = "Plugin template not found at: ";
            OutError.append(TemplatePath.c_str(), TemplatePath.size());
            return false;
        }

        // Reject collisions with any already-discovered plugin (engine or project);
        // plugin names must be globally unique or discovery silently drops one.
        const FString NameStr(NewPluginName.data(), NewPluginName.size());
        if (FPluginManager::Get().FindPlugin(NameStr) != nullptr)
        {
            OutError = "A plugin named '";
            OutError += NameStr;
            OutError += "' already exists.";
            return false;
        }

        // Destination: <ProjectPath>/Plugins/<PluginName>/
        const FFixedString PluginDir = Paths::Combine(Paths::Combine(GetProjectPath(), "Plugins"), NameStr);

        if (Filesystem::Exists(PluginDir) && !Filesystem::IsDirectoryEmpty(PluginDir))
        {
            OutError = "A non-empty folder already exists at: ";
            OutError.append(PluginDir.c_str(), PluginDir.size());
            return false;
        }

        auto ToUpper = [](FString& S)
        {
            std::transform(S.begin(), S.end(), S.begin(),
                [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        };

        FPluginTemplateContext Ctx;
        Ctx.Name = NameStr;
        Ctx.Description.assign(Description.data(), Description.size());
        if (Ctx.Description.empty())
        {
            Ctx.Description = "A Lumina plugin";
        }
        Ctx.NameUpper = Ctx.Name;
        ToUpper(Ctx.NameUpper);
        Ctx.RuntimeModule = Ctx.Name; Ctx.RuntimeModule += "Runtime";
        Ctx.EditorModule  = Ctx.Name; Ctx.EditorModule  += "Editor";
        Ctx.RuntimeModuleUpper = Ctx.RuntimeModule; ToUpper(Ctx.RuntimeModuleUpper);
        Ctx.EditorModuleUpper  = Ctx.EditorModule;  ToUpper(Ctx.EditorModuleUpper);

        if (!CopyTemplateTree(TemplatePath, PluginDir,
            [&Ctx](FString& Text) { ReplacePluginTokens(Text, Ctx); },
            OutError))
        {
            return false;
        }

        OutPluginDir = PluginDir;
        return true;
    }

    bool FEditorEngine::GenerateProjectFiles(FStringView ProjectDirectory) const
    {
        // Both scripts ship in every generated project, because a project is not tied to the
        // platform it was created on. Which one to run is decided by the host: handing a .bat to
        // a POSIX exec fails with "permission denied", which describes nothing about the problem.
#if defined(_WIN32)
        constexpr const char* ScriptName = "GenerateProject.bat";
#else
        constexpr const char* ScriptName = "GenerateProject.sh";
#endif

        FFixedString ScriptPath = Paths::Combine(ProjectDirectory, ScriptName);
        if (!Paths::Exists(ScriptPath))
        {
            LOG_ERROR("GenerateProjectFiles: missing {0}", ScriptPath.c_str());
            return false;
        }

        // Detached worker thread runs the build tool, captures stdout+stderr, and
        // streams each line into the editor log under a [BuildTool] tag so the
        // user sees what's happening without a separate console window. The
        // FScopedSlowTask drives a centered progress modal for the duration.
        const std::string ScriptPathStr(ScriptPath.c_str(), ScriptPath.size());
        const std::string WorkingDirStr(ProjectDirectory.data(), ProjectDirectory.size());

        std::thread([ScriptPathStr, WorkingDirStr]()
        {
            FScopedSlowTask Task(1.0f, "Generating project files", "Running LuminaBuildTool GenerateProjectFiles...");

            LOG_INFO("[BuildTool] running {0}", ScriptPathStr.c_str());

            const int ExitCode = Platform::RunProcessAndWaitCapture(
                UTF8_TO_TCHAR(ScriptPathStr.c_str()),
                nullptr,
                UTF8_TO_TCHAR(WorkingDirStr.c_str()),
                [](FStringView Line)
                {
                    if (Line.empty())
                    {
                        return;
                    }
                    LOG_INFO("[BuildTool] {0}", FString(Line.data(), Line.size()).c_str());
                });

            if (ExitCode == 0)
            {
                LOG_INFO("[BuildTool] project files generated successfully.");
            }
            else
            {
                LOG_ERROR("[BuildTool] generation failed (exit code {0}).", ExitCode);
            }
        }).detach();

        return true;
    }
}
