#pragma once

namespace Lumina
{
    class CFont;

    // Bakes an MSDF (MTSDF, RGBA8) atlas for the standard printable charset into the font, from the TTF/OTF
    // bytes already in CFont::FontData. Fills Glyphs / AtlasPixels / dims / metrics. Returns false if the
    // face has no data or no glyphs. Used by the editor importer and the runtime default-font manager.
    RUNTIME_API bool BakeFontAtlas(CFont* Font);
}
