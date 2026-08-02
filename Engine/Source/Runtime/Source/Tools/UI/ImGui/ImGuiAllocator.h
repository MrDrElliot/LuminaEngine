#pragma once
#include <imgui.h>
#include "Memory/Memory.h"
#include "Memory/MemoryTracking.h"

namespace Lumina::ImGuiX
{
    inline void* ImGuiMemAlloc(size_t Size, void*)
    {
        LUMINA_MEMORY_SCOPE("ImGui");
        return Memory::Malloc(Size);
    }

    inline void ImGuiMemFree(void* Ptr, void*)
    {
        if (Ptr)
        {
            Memory::Free(Ptr);
        }
    }

    inline void InstallImGuiAllocator()
    {
        ImGui::SetAllocatorFunctions(&ImGuiMemAlloc, &ImGuiMemFree);
    }
}
