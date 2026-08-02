#pragma once

#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectMacros.h"
#include "FontManager.generated.h"

namespace Lumina
{
    class CFont;

    // Holds engine-default font assets, constructed at runtime (mirrors CPrimitiveManager). World text with
    // no font assigned -- or one whose atlas failed to bake -- falls back to DefaultFont.
    REFLECT()
    class RUNTIME_API CFontManager : public CObject
    {
        GENERATED_BODY()
    public:

        CFontManager();

        void Initialize();

        static CFontManager& Get();

        CFont* GetDefaultFont() const { return DefaultFont; }

        /** Default engine font (Lexend), MSDF-baked at startup. */
        PROPERTY(NotSerialized)
        TObjectPtr<CFont> DefaultFont;
    };
}
