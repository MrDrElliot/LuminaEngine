#pragma once

#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectMacros.h"
#include "FontManager.generated.h"

namespace Lumina
{
    class CFont;

    // Holds engine-default font assets, and world text with no usable font falls back to DefaultFont.
    REFLECT()
    class RUNTIME_API CFontManager : public CObject
    {
        GENERATED_BODY()
    public:

        CFontManager();

        void Initialize();

        static CFontManager& Get();

        CFont* GetDefaultFont() const { return DefaultFont.Get(); }

        /** Default engine font (Lexend), MSDF-baked at startup. */
        PROPERTY(Hidden, NoSerialize)
        TStrongObjectPtr<CFont> DefaultFont;
    };
}
