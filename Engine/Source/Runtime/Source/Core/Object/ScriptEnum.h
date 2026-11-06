#pragma once

#include "Class.h"
#include "Cast.h"


namespace Lumina
{
    // An enum minted at runtime from a C# type, so asking whether an enum is script-defined is a cast.
    class LUMINA_VISIBLE_TYPE CScriptEnum : public CEnum
    {
    public:

        DECLARE_CLASS(Lumina, CScriptEnum, CEnum, "/Types/Engine", RUNTIME_API)
        DEFINE_CLASS_FACTORY(CScriptEnum)

        CScriptEnum() = default;

        // The C# underlying integer type, which is the width a field of this enum reserves.
        EPropertyTypeFlags UnderlyingType = EPropertyTypeFlags::Int32;
    };

    // Null unless the enum was minted from a script type, mirroring ToScriptClass.
    FORCEINLINE const CScriptEnum* ToScriptEnum(const CEnum* Enum)
    {
        return (Enum != nullptr && Enum->GetClass() != nullptr) ? Cast<CScriptEnum>(Enum) : nullptr;
    }

    FORCEINLINE CScriptEnum* ToScriptEnum(CEnum* Enum)
    {
        return (Enum != nullptr && Enum->GetClass() != nullptr) ? Cast<CScriptEnum>(Enum) : nullptr;
    }
}
