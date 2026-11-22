#pragma once

namespace Lumina
{
    class CFont;

    // Bakes an RGBA8 MTSDF atlas for the printable charset from CFont::FontData, false if the face has no data or glyphs.
    RUNTIME_API bool BakeFontAtlas(CFont* Font);
}
