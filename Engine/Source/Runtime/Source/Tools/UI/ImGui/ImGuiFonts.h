#pragma once

#include "imgui.h"
#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"


namespace Lumina::ImGuiX::Font
{
    enum class EFont : uint8
    {
        Tiny,
        TinyBold,
        Small,
        SmallBold,
        Medium,
        MediumBold,
        Large,
        LargeBold,
        Mono,
        MonoBold,

        NumFonts,
        Default = Medium,
    };
    
    RUNTIME_API extern ImFont* GFonts[static_cast<int32>(EFont::NumFonts)];

    RUNTIME_API FORCEINLINE void PushFont(EFont font) 
    {
        ImFont* Font = GFonts[static_cast<int8>(font)];
        ImGui::PushFont(Font); 
    }

    RUNTIME_API FORCEINLINE void PopFont() { ImGui::PopFont(); }
}

