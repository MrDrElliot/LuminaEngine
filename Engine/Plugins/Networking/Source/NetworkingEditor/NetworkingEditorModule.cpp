#include "NetworkingEditorModule.h"

// Engine headers reach for entt; the Runtime PCH supplies it there, not here.
#include <entt/entt.hpp>

#include "NetworkEditorTool.h"
#include "Core/Engine/Engine.h"
#include "Core/Module/ModuleManager.h"
#include "LuminaEditor.h"
#include "Log/Log.h"
#include "UI/EditorUI.h"
#include "UI/Tools/ToolsMenuRegistry.h"

using namespace Lumina;

IMPLEMENT_MODULE(FNetworkingEditorModule, "NetworkingEditor");

namespace
{
    FEditorUI* GetEditorUI()
    {
        if (GEditorEngine == nullptr)
        {
            return nullptr;
        }
        return static_cast<FEditorUI*>(GEditorEngine->GetDevelopmentToolsUI());
    }
}

void FNetworkingEditorModule::StartupModule()
{
    FToolsMenuEntry Entry;
    Entry.Label    = LE_ICON_LAN " Network";
    Entry.IsActive = []() -> bool
    {
        FEditorUI* UI = GetEditorUI();
        return UI != nullptr && UI->IsToolActive<FNetworkEditorTool>();
    };
    Entry.OnToggle = []()
    {
        if (FEditorUI* UI = GetEditorUI())
        {
            UI->ToggleTool<FNetworkEditorTool>(UI);
        }
    };

    ToolsMenuHandle = FToolsMenuRegistry::Get().Register(Move(Entry));
}

void FNetworkingEditorModule::ShutdownModule()
{
    // Before the DLL goes: the entry holds callables whose code lives here.
    FToolsMenuRegistry::Get().Unregister(ToolsMenuHandle);
    ToolsMenuHandle = 0;
}
