#pragma once

#include "Core/Engine/Engine.h"
#include "Platform/Process/PlatformProcess.h"

namespace Lumina
{
    class CWorld;
    class FCamera;
    class FEditorSettings;
    class FEditorLayer;
    class FEditorPanel;

    EDITOR_API extern class FEditorEngine* GEditorEngine;
    
    class EDITOR_API FEditorEngine : public FEngine
    {
    public:
        bool Init() override;
        bool Shutdown() override;

        CWorld* GetCurrentEditorWorld() const;

        #if WITH_EDITOR
        IDevelopmentToolUI* CreateDevelopmentTools() override;
        #endif

        /**
         * Creates a new project on disk from the Blank template.
         * Returns true on success and fills OutProjectFile with the absolute
         * path to the generated .lproject. On failure, returns false and
         * writes a human-readable reason into OutError.
         */
        bool CreateProject(FStringView NewProjectName, FStringView NewProjectPath, FFixedString& OutProjectFile, FString& OutError);

        /**
         * Scaffolds a new project-local plugin from the Plugin template into
         * <ProjectPath>/Plugins/<NewPluginName>/, emitting the .lplugin and a
         * Runtime + Editor module pair with their Build.cs. Requires a loaded
         * project. Returns true and fills OutPluginDir with the plugin folder;
         * on failure returns false with a reason in OutError. Regenerate the
         * project (GenerateProjectFiles) afterward so the new modules build.
         */
        bool CreatePlugin(FStringView NewPluginName, FStringView Description, FFixedString& OutPluginDir, FString& OutError);

        // Runs this engine's build tool on a worker and streams its output to the log. False when it cannot start.
        bool GenerateProjectFiles(FStringView ProjectDirectory) const;

        // Claims the --Project one before any window opens. False when another editor has it, which then gets focus.
        bool ClaimStartupProject();

    protected:

        /** Editor doesn't auto-load a runtime world; the user picks one via the editor UI. */
        void LoadStartupMap() override {}

        bool ClaimProject(FStringView ProjectDirectory) override;

    private:

        Platform::FInstanceLock ProjectLock;
        FFixedString            LockedProjectDirectory;
    };
    
    
    
}
