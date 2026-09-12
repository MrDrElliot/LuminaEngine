#pragma once

#include "Class.h"
#include "Cast.h"

RUNTIME_API Lumina::CClass* Construct_CClass_Lumina_CEnum();

namespace Lumina
{
    /**
     * An enum whose members were decided at runtime, from a C# type rather than a C++ declaration.
     *
     * Exists for the same reason CScriptClass does: "was this minted from a script type" becomes a cast
     * rather than a string lookup in metadata, and the things only a minted enum has stop being carried by
     * every native CEnum in the engine.
     */
    class LUMINA_VISIBLE_TYPE CScriptEnum : public CEnum
    {
    public:

        DECLARE_CLASS(Lumina, CScriptEnum, CEnum, "/Script/Engine", RUNTIME_API)
        DEFINE_CLASS_FACTORY(CScriptEnum)

        CScriptEnum() = default;

        /**
         * The C# type name this was minted from.
         *
         * Kept because the engine-side NAME is not a reliable identity: reflected type names share one flat
         * space, so a script enum colliding with a native one is minted under a generated name instead. That
         * used to discard the C# identity outright, leaving saved data that referenced it with nothing to
         * redirect through.
         */
        FName ScriptTypeName;

        /** The C# underlying integer type, which is the width a field of this enum reserves. */
        EPropertyTypeFlags UnderlyingType = EPropertyTypeFlags::Int32;
    };

    /** Null unless the enum was minted from a script type. Mirrors ToScriptClass. */
    FORCEINLINE const CScriptEnum* ToScriptEnum(const CEnum* Enum)
    {
        return (Enum != nullptr && Enum->GetClass() != nullptr) ? Cast<CScriptEnum>(Enum) : nullptr;
    }

    FORCEINLINE CScriptEnum* ToScriptEnum(CEnum* Enum)
    {
        return (Enum != nullptr && Enum->GetClass() != nullptr) ? Cast<CScriptEnum>(Enum) : nullptr;
    }
}
