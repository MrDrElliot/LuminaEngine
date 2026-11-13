#include "RuntimePCH.h"
#include "ImGuiFonts.h"
#include "Platform/GenericPlatform.h"

namespace Lumina::Global
{
    ImFont* GFonts[static_cast<int32>(ImGuiX::Font::EFont::NumFonts)] = {};
}
