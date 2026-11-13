#pragma once

#include "Config/DeveloperSettings.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectMacros.h"
#include "Input/InputMode.h"
#include "UISettings.generated.h"

namespace Lumina
{
    // One font file the UI registers under a family name, which stylesheets then select with font-family.
    REFLECT()
    struct RUNTIME_API SUIFontFace
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Font")
        FString Family;

        // A .ttf or .otf under a mounted folder, such as /Game/Content/UI/Fonts/Title.ttf.
        PROPERTY(Editable, Category = "Font")
        FString Path;

        PROPERTY(Editable, Category = "Font")
        bool bBold = false;

        PROPERTY(Editable, Category = "Font")
        bool bItalic = false;
    };

    // The image the OS draws for one cursor type, as Unreal's hardware cursors do.
    REFLECT()
    struct RUNTIME_API SHardwareCursor
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Cursor")
        EMouseCursor Cursor = EMouseCursor::Default;

        // A PNG given relative to the project or as a /Game path, such as /Game/Cursors/Pointer.png.
        PROPERTY(Editable, FilePath, Category = "Cursor")
        FString Image;

        // Where the cursor clicks, from 0,0 at the image's top left to 1,1 at its bottom right.
        PROPERTY(Editable, Category = "Cursor", ClampMin = 0.0f, ClampMax = 1.0f)
        FVector2 Hotspot = FVector2(0.0f);
    };

    // Project-wide user interface configuration.
    REFLECT(MinimalAPI, ConfigFile = "/Config/UISettings.json", DisplayName = "User Interface", Category = "Project")
    class CUISettings : public CDeveloperSettings
    {
        GENERATED_BODY()
    public:

        // Fonts the project's documents can use by family name.
        PROPERTY(Editable, Category = "Fonts")
        TVector<SUIFontFace> FontFaces;

        // Cursor types the OS draws from an image over the game, so they never lag the mouse. A type left out shows the system's own.
        PROPERTY(Editable, Category = "Hardware Cursors")
        TVector<SHardwareCursor> HardwareCursors;

        RUNTIME_API const SHardwareCursor* FindHardwareCursor(EMouseCursor Cursor) const;

        void PostInitSettings() override;
        void PostPropertyChange(FProperty* ChangedProperty) override;
    };
}
