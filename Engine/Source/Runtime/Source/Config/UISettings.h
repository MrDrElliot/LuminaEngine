#pragma once

#include "Config/DeveloperSettings.h"
#include "Core/Object/ObjectMacros.h"
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

    // Project-wide user interface configuration.
    REFLECT(MinimalAPI, ConfigFile = "/Config/UISettings.json", DisplayName = "User Interface", Category = "Project")
    class CUISettings : public CDeveloperSettings
    {
        GENERATED_BODY()
    public:

        // Fonts the project's documents can use by family name.
        PROPERTY(Editable, Category = "Fonts")
        TVector<SUIFontFace> FontFaces;

        void PostInitSettings() override;
        void PostPropertyChange(FProperty* ChangedProperty) override;
    };
}
