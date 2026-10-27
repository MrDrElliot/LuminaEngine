#include "EditorModule.h"

#include "Core/Module/ModuleManager.h"
#include "Tools/UI/ImGui/ImGuiModule.h"


IMPLEMENT_MODULE(Lumina::FEditorModule, "EditorModule");

// The editor sets its own context at startup, and this hook is how a test binary hands it one.
LUMINA_MODULE_IMGUI();
