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
}

namespace Lumina::Global
{
    RUNTIME_API extern ImFont* GFonts[static_cast<int32>(ImGuiX::Font::EFont::NumFonts)];
}

namespace Lumina::ImGuiX::Font
{
    RUNTIME_API FORCEINLINE void PushFont(EFont font) 
    {
        ImFont* Font = Global::GFonts[static_cast<int8>(font)];
        ImGui::PushFont(Font); 
    }

    RUNTIME_API FORCEINLINE void PopFont() { ImGui::PopFont(); }
}

