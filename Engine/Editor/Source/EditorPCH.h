#pragma once

// Keep this lean, since every Editor TU pays for what is pulled in here and any edit invalidates the PCH.

// The build includes ModuleAPI after the PCH header, so it is included first to define LUMINA_SCRIPT_API for interop thunks.
#include "ModuleAPI.h"
#include "RuntimePCH.h"

// ImGui is touched by ~25 Editor TUs directly and far more transitively.
#include <imgui.h>
#include <imgui_internal.h>

// ImGuiX is included by most Editor TUs, so hoisting it here replaces many redundant parses.
#include "Tools/UI/ImGui/ImGuiX.h"
