#pragma once

#include "Core/Object/ObjectMacros.h"
#include "Platform/GenericPlatform.h"
#include "UITypes.generated.h"

namespace Lumina
{
    // Numbers cross as double and are coerced back to this type, so {{ Health }} formats as an int.
    REFLECT()
    enum class EUIVarType : int32
    {
        Bool   = 0,
        Int    = 1,
        Float  = 2,
        Double = 3,
        String = 4,
    };

    // One reflected uint64 is how a non-reflectable RmlUi pointer reaches script by value.

    /** An Rml::ElementDocument* loaded into a world's screen context. Valid until it is unloaded. */
    REFLECT()
    struct RUNTIME_API FUIDocument
    {
        GENERATED_BODY()

        PROPERTY()
        uint64 Handle = 0;
    };

    /** An Rml::Element* inside a loaded document. Valid while that document is loaded. */
    REFLECT()
    struct RUNTIME_API FUIElement
    {
        GENERATED_BODY()

        PROPERTY()
        uint64 Handle = 0;
    };

    /** One element event listener, owned by the UI bridge and reaped with the world's UI. */
    REFLECT()
    struct RUNTIME_API FUIEventListener
    {
        GENERATED_BODY()

        PROPERTY()
        uint64 Handle = 0;
    };

    /** A named data model registered on a world's UI context. */
    REFLECT()
    struct RUNTIME_API FUIDataModel
    {
        GENERATED_BODY()

        PROPERTY()
        uint64 Handle = 0;
    };
}
