#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"

namespace Lumina
{
    struct FCodeEditorPalette;

    // Code editor color themes, one JSON file each in the editor's resources, which a packaged game never carries.
    namespace CodeEditorThemes
    {
        inline constexpr const char* Directory = "/Editor/Resources/CodeThemes";

        EDITOR_API TVector<FString> List();
        EDITOR_API FString PathOf(FStringView Name);

        // Overlays the named theme on Palette, false when the file is missing or is not JSON.
        EDITOR_API bool Load(FStringView Name, FCodeEditorPalette& Palette);

        // Writes Palette as a theme file, every color included, so a new theme starts from a complete copy.
        EDITOR_API bool Save(FStringView Name, const FCodeEditorPalette& Palette);
    }
}
