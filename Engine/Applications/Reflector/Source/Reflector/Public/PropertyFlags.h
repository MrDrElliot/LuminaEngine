#pragma once

// Spells out its own integer types rather than relying on whoever includes it first to have pulled them.
#include <cstdint>
#include <string>
#include <string_view>

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
inline constexpr Enum  operator~ (Enum  E)             { return (Enum)~(__underlying_type(Enum))E; } \
static_assert(true, "ENUM_CLASS_FLAGS consumes the semicolon at the call site")

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


    // Kept in the order of the runtime's EPropertyTypeFlags.inl, since the Reflector cannot include runtime headers.
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
        Entity,
        ComponentRef,
    };

    // The flags the Reflector sets, at the bits the runtime's EPropertyFlags.inl gives them.
    enum class EPropertyFlags : uint32_t
    {
        None            = 0,
        Editable        = BIT(0),
        ReadOnly        = BIT(1),
        NoSerialize     = BIT(2),
        Const           = BIT(3),
        Private         = BIT(4),
        Protected       = BIT(5),
        EditorOnly      = BIT(11),
        Replicated      = BIT(12),
        ScriptReadOnly  = BIT(13),
        ScriptWritable  = BIT(14),
        ScriptHidden    = BIT(15),
        DuplicateTransient = BIT(16),
        SaveGame        = BIT(19),
    };

    ENUM_CLASS_FLAGS(EPropertyFlags);

    inline std::string PropertyFlagsToString(EPropertyFlags Flags)
    {
        if (Flags == EPropertyFlags::None)
        {
            return "Lumina::EPropertyFlags::None";
        }

        std::string Result;

        auto AppendFlag = [&](EPropertyFlags Flag, std::string_view Name)
            {
                if ((Flags & Flag) != EPropertyFlags::None)
                {
                    if (!Result.empty())
                    {
                        Result += " | ";
                    }
                    Result.append(Name.data(), Name.length());
                }
            };

        AppendFlag(EPropertyFlags::Editable, "Lumina::EPropertyFlags::Editable");
        AppendFlag(EPropertyFlags::ReadOnly, "Lumina::EPropertyFlags::ReadOnly");
        AppendFlag(EPropertyFlags::NoSerialize, "Lumina::EPropertyFlags::NoSerialize");
        AppendFlag(EPropertyFlags::Const, "Lumina::EPropertyFlags::Const");
        AppendFlag(EPropertyFlags::Private, "Lumina::EPropertyFlags::Private");
        AppendFlag(EPropertyFlags::Protected, "Lumina::EPropertyFlags::Protected");
        AppendFlag(EPropertyFlags::EditorOnly, "Lumina::EPropertyFlags::EditorOnly");
        AppendFlag(EPropertyFlags::Replicated, "Lumina::EPropertyFlags::Replicated");
        AppendFlag(EPropertyFlags::ScriptReadOnly, "Lumina::EPropertyFlags::ScriptReadOnly");
        AppendFlag(EPropertyFlags::ScriptWritable, "Lumina::EPropertyFlags::ScriptWritable");
        AppendFlag(EPropertyFlags::ScriptHidden, "Lumina::EPropertyFlags::ScriptHidden");
        AppendFlag(EPropertyFlags::DuplicateTransient, "Lumina::EPropertyFlags::DuplicateTransient");
        AppendFlag(EPropertyFlags::SaveGame, "Lumina::EPropertyFlags::SaveGame");

        return Result;
    }

    // Which reflected type a property names, so the generated file declares its accessor before the trait reaches for it.
    enum class EReflectedLink : uint8_t
    {
        None,
        Struct,
        Class,
        Enum,
    };
}
