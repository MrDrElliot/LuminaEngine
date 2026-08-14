#pragma once

namespace Lumina
{
    
#define BIT(x) (1 << (x))

#define ENUM_CLASS_FLAGS(Enum) \
inline           Enum& operator|=(Enum& Lhs, Enum Rhs) { return Lhs = (Enum)((__underlying_type(Enum))Lhs | (__underlying_type(Enum))Rhs); } \
inline           Enum& operator&=(Enum& Lhs, Enum Rhs) { return Lhs = (Enum)((__underlying_type(Enum))Lhs & (__underlying_type(Enum))Rhs); } \
inline           Enum& operator^=(Enum& Lhs, Enum Rhs) { return Lhs = (Enum)((__underlying_type(Enum))Lhs ^ (__underlying_type(Enum))Rhs); } \
inline constexpr Enum  operator| (Enum  Lhs, Enum Rhs) { return (Enum)((__underlying_type(Enum))Lhs | (__underlying_type(Enum))Rhs); } \
inline constexpr Enum  operator& (Enum  Lhs, Enum Rhs) { return (Enum)((__underlying_type(Enum))Lhs & (__underlying_type(Enum))Rhs); } \
inline constexpr Enum  operator^ (Enum  Lhs, Enum Rhs) { return (Enum)((__underlying_type(Enum))Lhs ^ (__underlying_type(Enum))Rhs); } \
inline constexpr bool  operator! (Enum  E)             { return !(__underlying_type(Enum))E; } \
inline constexpr Enum  operator~ (Enum  E)             { return (Enum)~(__underlying_type(Enum))E; }

template<typename Enum>
constexpr bool EnumHasAllFlags(Enum Flags, Enum Contains)
{
    using UnderlyingType = __underlying_type(Enum);
    return ((UnderlyingType)Flags & (UnderlyingType)Contains) == (UnderlyingType)Contains;
}

template<typename Enum>
constexpr bool EnumHasAnyFlags(Enum Flags, Enum Contains)
{
    using UnderlyingType = __underlying_type(Enum);
    return ((UnderlyingType)Flags & (UnderlyingType)Contains) != 0;
}

template<typename Enum>
void EnumAddFlags(Enum& Flags, Enum FlagsToAdd)
{
    using UnderlyingType = __underlying_type(Enum);
    Flags = (Enum)((UnderlyingType)Flags | (UnderlyingType)FlagsToAdd);
}

template<typename Enum>
void EnumRemoveFlags(Enum& Flags, Enum FlagsToRemove)
{
    using UnderlyingType = __underlying_type(Enum);
    Flags = (Enum)((UnderlyingType)Flags & ~(UnderlyingType)FlagsToRemove);
}


    /** This must reflect EPropertyTypeFlags found in ObjectCore.h */
    enum class EPropertyTypeFlags : uint64_t
    {
        None = 0,

        // Signed integers
        Int8,
        Int16,
        Int32,
        Int64,

        // Unsigned integers
        UInt8,
        UInt16,
        UInt32,
        UInt64,

        // Floats
        Float,
        Double,

        // Other types
        Bool,
        Object,
        SoftObject,
        Class,
        Name,
        String,
        Enum,
        Vector,
        Struct,
        Optional,
        SubStruct,
        Delegate,
        InstancedStruct,
        Map,
    };

    /** Must be in-sync with EPropertyFlags in ObjectCore.h */
    enum class EPropertyFlags : uint16_t
    {
        None            = 0,
        Editable        = BIT(0),
        ReadOnly        = BIT(1),
        NoSerialize     = BIT(2),
        Const           = BIT(3),
        Private         = BIT(4),
        Protected       = BIT(5),
        SubField        = BIT(6),
        Trivial         = BIT(7),
        Script          = BIT(8),
        Builtin         = BIT(9),
        BulkSerialize   = BIT(10),
        // Property exists only for editor tooling. Stripped from cooked
        // packages (see CStruct::SerializeTaggedProperties + FArchive::IsCooking).
        EditorOnly      = BIT(11),
        // Property participates in network replication (PROPERTY(Replicated)).
        Replicated      = BIT(12),
        // Script (C#) interop specifiers, independent of the editor flags above.
        ScriptReadOnly  = BIT(13),
        ScriptWritable  = BIT(14),
        ScriptHidden    = BIT(15),
    };

    ENUM_CLASS_FLAGS(EPropertyFlags)

    inline eastl::string PropertyFlagsToString(EPropertyFlags Flags)
    {
        if (Flags == EPropertyFlags::None)
        {
            return "Lumina::EPropertyFlags::None";
        }

        eastl::string Result;

        auto AppendFlag = [&](EPropertyFlags Flag, eastl::string_view Name)
            {
                if ((Flags & Flag) != EPropertyFlags::None)
                {
                    if (!Result.empty())
                    {
                        Result += " | ";
                    }
                    Result.append_convert(Name.begin(), Name.length());
                }
            };

        AppendFlag(EPropertyFlags::Editable, "Lumina::EPropertyFlags::Editable");
        AppendFlag(EPropertyFlags::ReadOnly, "Lumina::EPropertyFlags::ReadOnly");
        AppendFlag(EPropertyFlags::NoSerialize, "Lumina::EPropertyFlags::NoSerialize");
        AppendFlag(EPropertyFlags::Const, "Lumina::EPropertyFlags::Const");
        AppendFlag(EPropertyFlags::Private, "Lumina::EPropertyFlags::Private");
        AppendFlag(EPropertyFlags::Protected, "Lumina::EPropertyFlags::Protected");
        AppendFlag(EPropertyFlags::SubField, "Lumina::EPropertyFlags::SubField");
        AppendFlag(EPropertyFlags::Trivial, "Lumina::EPropertyFlags::Trivial");
        AppendFlag(EPropertyFlags::Script, "Lumina::EPropertyFlags::Script");
        AppendFlag(EPropertyFlags::Builtin, "Lumina::EPropertyFlags::Builtin");
        AppendFlag(EPropertyFlags::BulkSerialize, "Lumina::EPropertyFlags::BulkSerialize");
        AppendFlag(EPropertyFlags::EditorOnly, "Lumina::EPropertyFlags::EditorOnly");
        AppendFlag(EPropertyFlags::Replicated, "Lumina::EPropertyFlags::Replicated");
        AppendFlag(EPropertyFlags::ScriptReadOnly, "Lumina::EPropertyFlags::ScriptReadOnly");
        AppendFlag(EPropertyFlags::ScriptWritable, "Lumina::EPropertyFlags::ScriptWritable");
        AppendFlag(EPropertyFlags::ScriptHidden, "Lumina::EPropertyFlags::ScriptHidden");

        return Result;
    }
}
