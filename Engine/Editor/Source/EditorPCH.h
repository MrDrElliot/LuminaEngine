#pragma once

// Editor PCH. Includes Runtime's PCH (which carries the standard library, our containers, entt and xxhash)
// plus the editor-only heavies (ImGui).
//
// Keep this lean: anything pulled in here is paid by all 95 Editor TUs every
// build, and any edit invalidates the PCH. Editor-specific headers
// (EditorUI, EditorTool, etc.) intentionally stay out so that touching one
// of them doesn't dirty the entire PCH.

// ModuleAPI carries LUMINA_SCRIPT_API. The build /FI-includes it AFTER the PCH
// header, so anything the PCH pulls in that declares an interop thunk would see
// it undefined; including it up-front here settles that before the PCH is parsed.
// The per-module RUNTIME_API / EDITOR_API macros do not come from here: the build
// system defines them on the command line, so they are already in scope.
#include "ModuleAPI.h"
#include "RuntimePCH.h"

// ImGui is touched by ~25 Editor TUs directly and far more transitively.
#include <imgui.h>
#include <imgui_internal.h>

// ImGuiX is included by 29+ Editor TUs and transitively pulls imgui_internal,
// ImGuizmo, AssetRegistry, and a handful of Containers. Hoisting it
// into the PCH replaces ~29 redundant parses with one.
#include "Tools/UI/ImGui/ImGuiX.h"
