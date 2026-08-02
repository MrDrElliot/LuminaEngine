#include "EditorPCH.h"
#include "EditorModule.h"

#include "Core/Module/ModuleManager.h"
#include "Memory/Memory.h"

namespace Lumina
{
    void FEditorModule::StartupModule()
    {
        IModuleInterface::StartupModule();
    }

    void FEditorModule::ShutdownModule()
    {
        IModuleInterface::ShutdownModule();
    }
}

IMPLEMENT_MODULE(Lumina::FEditorModule, "EditorModule");
