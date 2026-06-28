#include "LuminaEditor.h"
#include <filesystem>
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
        // premake hard-fails without it. Process-local set covers tools we spawn
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
        std::error_code CreateEc;
        std::filesystem::create_directories(DestDir.c_str(), CreateEc);
        if (CreateEc)
        {
            OutError = "Failed to create directory: ";
            OutError.append(CreateEc.message().c_str());
            return false;
        }

        for (auto& Entry : std::filesystem::recursive_directory_iterator(TemplateDir.c_str()))
        {
            std::filesystem::path RelativePath = std::filesystem::relative(Entry.path(), TemplateDir.c_str());

            FString RelativePathStr = RelativePath.string().c_str();
            eastl::replace(RelativePathStr.begin(), RelativePathStr.end(), '\\', '/');

            ReplaceTokens(RelativePathStr);
            FFixedString DestPath = Paths::Combine(DestDir, RelativePathStr);

            if (Entry.is_directory())
            {
                std::filesystem::create_directories(DestPath.c_str());
            }
            else if (Entry.is_regular_file())
            {
                std::filesystem::path DestPathFS(DestPath.c_str());
                std::filesystem::create_directories(DestPathFS.parent_path());

                const std::filesystem::path SourcePath = Entry.path();
                const std::string Ext = SourcePath.extension().string();

                // Token replace only on text files; binaries (premake5.exe, etc.) copy verbatim.
                // .lua stays (premake5.lua carries $PROJECTNAME); .cs / .rml / .rcss are the C# + UI
                // authoring files of the current workflow.
                const bool bIsTextFile =
                    Ext == ".h"          || Ext == ".hpp"        || Ext == ".cpp"     ||
                    Ext == ".c"          || Ext == ".inl"        || Ext == ".lua"     ||
                    Ext == ".cs"         || Ext == ".rml"        || Ext == ".rcss"    ||
                    Ext == ".json"       || Ext == ".lproject"   || Ext == ".lplugin" ||
                    Ext == ".bat"        || Ext == ".py"         || Ext == ".md"      ||
                    Ext == ".txt"        || Ext == ".gitignore"  || Ext == ".cfg"     ||
                    Ext == ".yaml"       || Ext == ".yml"        || Ext == ".xml";

                if (!bIsTextFile)
                {
                    std::filesystem::copy_file(
                        SourcePath,
                        DestPath.c_str(),
                        std::filesystem::copy_options::overwrite_existing);
                    continue;
                }

                std::ifstream InputFile(SourcePath, std::ios::binary);
                if (!InputFile.is_open())
                {
                    continue;
                }

                std::string FileContentStr((std::istreambuf_iterator<char>(InputFile)), std::istreambuf_iterator<char>());
                InputFile.close();

                FString FileContents = FileContentStr.c_str();
                ReplaceTokens(FileContents);

                std::ofstream OutputFile(DestPath.c_str(), std::ios::binary);
                if (OutputFile.is_open())
                {
                    OutputFile.write(FileContents.data(), FileContents.size());
                    OutputFile.close();
                }
            }
        }

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
        std::error_code ParentEc;
        const FString ParentPathStr(NewProjectPath.data(), NewProjectPath.size());
        if (!std::filesystem::exists(ParentPathStr.c_str(), ParentEc))
        {
            OutError = "Project location does not exist: ";
            OutError.append(NewProjectPath.data(), NewProjectPath.size());
            return false;
        }

        std::error_code ExistEc;
        if (std::filesystem::exists(Combined.c_str(), ExistEc) &&
            !std::filesystem::is_empty(Combined.c_str(), ExistEc))
        {
            OutError = "A non-empty folder already exists at: ";
            OutError.append(Combined.c_str(), Combined.size());
            return false;
        }

        FProjectTemplateContext Ctx;
        Ctx.Name.assign(NewProjectName.data(), NewProjectName.size());
        Ctx.NameUpper = Ctx.Name;
        eastl::transform(
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
        eastl::replace(Ctx.LuminaDir.begin(), Ctx.LuminaDir.end(), '\\', '/');
        Ctx.LuminaDirBackslash = Ctx.LuminaDir;
        eastl::replace(Ctx.LuminaDirBackslash.begin(), Ctx.LuminaDirBackslash.end(), '/', '\\');

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

        std::error_code ExistEc;
        if (std::filesystem::exists(PluginDir.c_str(), ExistEc) &&
            !std::filesystem::is_empty(PluginDir.c_str(), ExistEc))
        {
            OutError = "A non-empty folder already exists at: ";
            OutError.append(PluginDir.c_str(), PluginDir.size());
            return false;
        }

        auto ToUpper = [](FString& S)
        {
            eastl::transform(S.begin(), S.end(), S.begin(),
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
        FFixedString BatPath = Paths::Combine(ProjectDirectory, "GenerateProject.bat");
        if (!Paths::Exists(BatPath))
        {
            LOG_ERROR("GenerateProjectFiles: missing {0}", BatPath.c_str());
            return false;
        }

        // Detached worker thread runs premake, captures stdout+stderr, and
        // streams each line into the editor log under a [premake] tag so the
        // user sees what's happening without a separate console window. The
        // FScopedSlowTask drives a centred progress modal for the duration.
        const std::string BatPathStr(BatPath.c_str(), BatPath.size());
        const std::string WorkingDirStr(ProjectDirectory.data(), ProjectDirectory.size());

        std::thread([BatPathStr, WorkingDirStr]()
        {
            FScopedSlowTask Task(1.0f, "Generating project files", "Running premake5 vs2026...");

            LOG_INFO("[premake] running {0}", BatPathStr.c_str());

            FWString WideExe = StringUtils::ToWideString(BatPathStr.c_str());
            FWString WideCwd = StringUtils::ToWideString(WorkingDirStr.c_str());

            const int ExitCode = Platform::RunProcessAndWaitCapture(
                WideExe.c_str(),
                nullptr,
                WideCwd.c_str(),
                [](FStringView Line)
                {
                    if (Line.empty())
                    {
                        return;
                    }
                    LOG_INFO("[premake] {0}", FString(Line.data(), Line.size()).c_str());
                });

            if (ExitCode == 0)
            {
                LOG_INFO("[premake] project files generated successfully.");
            }
            else
            {
                LOG_ERROR("[premake] generation failed (exit code {0}).", ExitCode);
            }
        }).detach();

        return true;
    }
}
