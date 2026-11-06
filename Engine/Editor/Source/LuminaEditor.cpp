#include "Core/Threading/Thread.h"
#include "LuminaEditor.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include <Core/Engine/Engine.h>
#include <Core/Plugin/PluginManager.h>
#include <Core/Progress/SlowTask.h>
#include <Memory/Memory.h>
#include <Tools/UI/DevelopmentToolUI.h>
#include "Config/Config.h"
#include "Core/CommandLine/CommandLine.h"
#include "FileSystem/FileSystem.h"
#include "GUID/GUID.h"
#include "Log/Log.h"
#include "Paths/Paths.h"
#include "Platform/Process/PlatformProcess.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/EditorUI.h"
#include "World/WorldManager.h"

namespace Lumina
{
    EDITOR_API FEditorEngine* GEditorEngine = nullptr;

    bool FEditorEngine::Init()
    {
        VFS::Mount<VFS::FNativeFileSystem>("/Editor", Paths::Combine(Paths::GetEngineDirectory(), "Editor"));

        GConfig->LoadPath("/Editor/Config");

        bool bSuccess = FEngine::Init();

        // Project settings under /Config load on a later pass, once a project mounts it.
        GConfig->DiscoverAndLoadSettings();


        return bSuccess;
    }

    bool FEditorEngine::Shutdown()
    {
        const bool bResult = FEngine::Shutdown();
        Platform::ReleaseInstanceLock(ProjectLock);
        return bResult;
    }

    bool FEditorEngine::ClaimStartupProject()
    {
        TOptional<FFixedString> Project = GCommandLine->Get("Project");
        if (!Project.IsSet())
        {
            return true;
        }

        const FFixedString Normalized = Paths::Normalize(FStringView(Project.GetValue()));
        return ClaimProject(Filesystem::IsDirectory(Normalized) ? FStringView(Normalized) : VFS::Parent(Normalized));
    }

    bool FEditorEngine::ClaimProject(FStringView ProjectDirectory)
    {
        // A benchmark opens the project read-only beside whoever is working in it.
        if (GCommandLine->Has("benchmark") || (ProjectLock.IsHeld() && Paths::PathsEqual(ProjectDirectory, LockedProjectDirectory)))
        {
            return true;
        }

        const FFixedString IntermediatesDirectory = Paths::Combine(ProjectDirectory, "Intermediates");
        Filesystem::MakeDirectoryTree(IntermediatesDirectory);

        Platform::FInstanceLock NewLock;
        uint32 OwnerId = 0;
        if (!Platform::TryAcquireInstanceLock(FString(*Paths::Combine(IntermediatesDirectory, "Editor.lock")), NewLock, OwnerId))
        {
            LOG_WARN("'{}' is already open in another editor (process {}), so it was not opened here.", ProjectDirectory, OwnerId);
            ImGuiX::Notifications::NotifyWarning("That project is already open in another editor window.");
            Platform::FocusProcessWindow(OwnerId);
            return false;
        }

        Platform::ReleaseInstanceLock(ProjectLock);
        ProjectLock = NewLock;
        LockedProjectDirectory = FStringView(ProjectDirectory.GetData(), ProjectDirectory.Len());
        return true;
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
            if (From.IsEmpty())
            {
                return;
            }
            int32 Pos = 0;
            while ((Pos = Str.Find(From, Pos)) != Constants::kIndexNone)
            {
                Str.RemoveAt(Pos, From.Len()).InsertAt(Pos, To);
                Pos += To.Len();
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

    // Precomputed so the template never composes tokens, which keeps replacement order-independent.
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
            if (From.IsEmpty())
            {
                return;
            }
            int32 Pos = 0;
            while ((Pos = Str.Find(From, Pos)) != Constants::kIndexNone)
            {
                Str.RemoveAt(Pos, From.Len()).InsertAt(Pos, To);
                Pos += To.Len();
            }
        };

        // Longest tokens first so a prefix such as $PLUGINNAME inside $PLUGINNAMEUPPER is not eaten early.
        ReplaceAll(Text, "$RUNTIMEMODULEUPPER", Ctx.RuntimeModuleUpper);
        ReplaceAll(Text, "$EDITORMODULEUPPER", Ctx.EditorModuleUpper);
        ReplaceAll(Text, "$PLUGINNAMEUPPER", Ctx.NameUpper);
        ReplaceAll(Text, "$PLUGINDESCRIPTION", Ctx.Description);
        ReplaceAll(Text, "$RUNTIMEMODULE", Ctx.RuntimeModule);
        ReplaceAll(Text, "$EDITORMODULE", Ctx.EditorModule);
        ReplaceAll(Text, "$PLUGINNAME", Ctx.Name);
    }

    // Relative paths are substituted too, so a $TOKEN filename works. Binary files copy verbatim.
    static bool CopyTemplateTree(
        const FFixedString&              TemplateDir,
        const FFixedString&              DestDir,
        const TFunction<void(FString&)>& ReplaceTokens,
        FString&                         OutError)
    {
        if (!Filesystem::MakeDirectoryTree(DestDir))
        {
            OutError = "Failed to create directory: ";
            OutError.Append(*DestDir, DestDir.Len());
            return false;
        }

        const int32 TemplateRootLength = TemplateDir.Len();

        Filesystem::IterateDirectoryRecursive(TemplateDir, [&](const Filesystem::FDirectoryEntry& Entry)
        {
            FString RelativePathStr(Entry.FullPath.Mid(TemplateRootLength + 1));

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
                
                const bool bIsTextFile =
                    Ext == ".h"          || Ext == ".hpp"        || Ext == ".cpp"     ||
                    Ext == ".c"          || Ext == ".inl"        ||
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

                const TSpan<const uint8> Bytes(reinterpret_cast<const uint8*>(FileContents.GetData()), FileContents.Len());
                if (Filesystem::WriteFile(DestPath, Bytes))
                {
                    // A fresh write drops the executable bit a generated GenerateProject.sh needs.
                    Filesystem::CopyPermissions(SourcePath, DestPath);
                }
            }
        });

        return true;
    }

    // It becomes the module name, the vcxproj name and part of source identifiers via the API macro.
    static bool ValidateProjectName(FStringView Name, FString& OutError)
    {
        if (Name.IsEmpty())
        {
            OutError = "Project name is empty.";
            return false;
        }

        const char First = Name[0];
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

        if (Name.Len() > 64)
        {
            OutError = "Project name is too long (max 64 chars).";
            return false;
        }

        return true;
    }

    bool FEditorEngine::CreateProject(FStringView NewProjectName, FStringView NewProjectPath, FFixedString& OutProjectFile, FString& OutError)
    {
        OutProjectFile.Reset();
        OutError.Reset();

        if (!ValidateProjectName(NewProjectName, OutError))
        {
            return false;
        }

        if (NewProjectPath.IsEmpty())
        {
            OutError = "Project location is empty.";
            return false;
        }

        const FString& EngineDir = Paths::GetEngineInstallDirectory();
        if (EngineDir.IsEmpty())
        {
            OutError = "The engine root could not be found beside this editor.";
            return false;
        }

        const FFixedString BlankProjectPath = Paths::Combine(EngineDir, "Templates", "Blank");
        if (!Paths::Exists(BlankProjectPath))
        {
            OutError = "Blank template not found at: ";
            OutError.Append(*BlankProjectPath, BlankProjectPath.Len());
            return false;
        }

        const FFixedString Combined = Paths::Combine(NewProjectPath, NewProjectName);
        if (!Filesystem::Exists(NewProjectPath))
        {
            OutError = "Project location does not exist: ";
            OutError.Append(NewProjectPath.GetData(), NewProjectPath.Len());
            return false;
        }

        if (Filesystem::Exists(Combined) && !Filesystem::IsDirectoryEmpty(Combined))
        {
            OutError = "A non-empty folder already exists at: ";
            OutError.Append(*Combined, Combined.Len());
            return false;
        }

        FProjectTemplateContext Ctx;
        Ctx.Name = FStringView(NewProjectName.GetData(), NewProjectName.Len());
        Ctx.NameUpper = Ctx.Name;
        Ctx.NameUpper.ToUpper();
        Ctx.Guid = FGuid::New().ToString(true, true);
        Ctx.Description = "A Lumina game project";
        Ctx.LuminaDir = EngineDir;
        Algo::Replace(Ctx.LuminaDir, '\\', '/');
        Ctx.LuminaDirBackslash = Ctx.LuminaDir;
        Algo::Replace(Ctx.LuminaDirBackslash, '/', '\\');

        if (!CopyTemplateTree(BlankProjectPath, Combined,
            [&Ctx](FString& Text) { ReplaceProjectTokens(Text, Ctx); },
            OutError))
        {
            return false;
        }

        OutProjectFile = Paths::Combine(Combined, FFixedString(*Ctx.Name).Append(".lproject"));
        return true;
    }

    bool FEditorEngine::CreatePlugin(FStringView NewPluginName, FStringView Description, FFixedString& OutPluginDir, FString& OutError)
    {
        OutPluginDir.Reset();
        OutError.Reset();

        if (!ValidateProjectName(NewPluginName, OutError))
        {
            return false;
        }

        // Project-local plugins live next to the .lproject; we need one loaded.
        if (GetProjectName().IsEmpty())
        {
            OutError = "No project is loaded. Open a project before creating a plugin.";
            return false;
        }

        const FString& EngineDir = Paths::GetEngineInstallDirectory();
        if (EngineDir.IsEmpty())
        {
            OutError = "The engine root could not be found beside this editor.";
            return false;
        }

        const FFixedString TemplatePath = Paths::Combine(EngineDir, "Templates", "Plugin");
        if (!Paths::Exists(TemplatePath))
        {
            OutError = "Plugin template not found at: ";
            OutError.Append(*TemplatePath, TemplatePath.Len());
            return false;
        }

        // Plugin names must be globally unique, or discovery silently drops one of the pair.
        const FString NameStr(NewPluginName.GetData(), NewPluginName.Len());
        if (FPluginManager::Get().FindPlugin(NameStr) != nullptr)
        {
            OutError = "A plugin named '";
            OutError += NameStr;
            OutError += "' already exists.";
            return false;
        }

        // Lands under the project's own Plugins directory.
        const FFixedString PluginDir = Paths::Combine(Paths::Combine(GetProjectPath(), "Plugins"), NameStr);

        if (Filesystem::Exists(PluginDir) && !Filesystem::IsDirectoryEmpty(PluginDir))
        {
            OutError = "A non-empty folder already exists at: ";
            OutError.Append(*PluginDir, PluginDir.Len());
            return false;
        }

        FPluginTemplateContext Ctx;
        Ctx.Name = NameStr;
        Ctx.Description = FStringView(Description.GetData(), Description.Len());
        if (Ctx.Description.IsEmpty())
        {
            Ctx.Description = "A Lumina plugin";
        }
        Ctx.NameUpper = Ctx.Name;
        Ctx.NameUpper.ToUpper();
        Ctx.RuntimeModule = Ctx.Name; Ctx.RuntimeModule += "Runtime";
        Ctx.EditorModule  = Ctx.Name; Ctx.EditorModule  += "Editor";
        Ctx.RuntimeModuleUpper = Ctx.RuntimeModule; Ctx.RuntimeModuleUpper.ToUpper();
        Ctx.EditorModuleUpper  = Ctx.EditorModule;  Ctx.EditorModuleUpper.ToUpper();

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
        // This editor's own engine, which hands the work on if the project names another.
        const FString Script = Paths::GetBuildToolScript();
        if (Script.IsEmpty())
        {
            LOG_ERROR("GenerateProjectFiles: this engine has no build tool, so project files cannot be generated.");
            return false;
        }

        // Streams each line under a [BuildTool] tag so the user sees progress without a console window.
        const FString Params = Format("GenerateProjectFiles -Project=\"{}\"", ProjectDirectory);
        const FString WorkingDirectory(ProjectDirectory.GetData(), ProjectDirectory.Len());

        FThread([Script, Params, WorkingDirectory]()
        {
            FScopedSlowTask Task(1.0f, "Generating project files", "Running LuminaBuildTool GenerateProjectFiles...");

            LOG_INFO("[BuildTool] running {0} {1}", *Script, *Params);

            const int ExitCode = Platform::RunProcessAndWaitCapture(
                UTF8_TO_TCHAR(*Script),
                UTF8_TO_TCHAR(*Params),
                UTF8_TO_TCHAR(*WorkingDirectory),
                [](FStringView Line)
                {
                    if (Line.IsEmpty())
                    {
                        return;
                    }
                    LOG_INFO("[BuildTool] {0}", *FString(Line.GetData(), Line.Len()));
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
