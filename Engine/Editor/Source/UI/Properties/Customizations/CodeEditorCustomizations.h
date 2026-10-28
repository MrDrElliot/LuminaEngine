#pragma once

#include "CoreTypeCustomization.h"
#include "Settings/EditorSettings.h"

namespace Lumina
{
    // A shortcut as two key fields in one row, the primary chord and the alternate.
    class FShortcutPropertyCustomization : public IPropertyTypeCustomization
    {
    public:

        static TSharedPtr<FShortcutPropertyCustomization> MakeInstance()
        {
            return MakeShared<FShortcutPropertyCustomization>();
        }

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

    private:

        SShortcut        CachedValue;
        SShortcut        DisplayValue;
        FKeyCaptureField PrimaryField;
        FKeyCaptureField AlternateField;
        bool             bFinishPending = false;
    };

    // Picks a code editor theme from the files in the CodeThemes folder, makes a new one from a typed name, and opens the file for editing.
    class FCodeEditorThemePropertyCustomization : public IPropertyTypeCustomization
    {
    public:

        static TSharedPtr<FCodeEditorThemePropertyCustomization> MakeInstance()
        {
            return MakeShared<FCodeEditorThemePropertyCustomization>();
        }

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

    private:

        SCodeEditorTheme CachedValue;
        SCodeEditorTheme DisplayValue;
        TVector<FString> Themes;
        double           NextListTime = 0.0;
        bool             bFinishPending = false;
    };
}
