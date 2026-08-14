#pragma once
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    enum class EEditorToolFlags : uint8
    {
        Tool_WantsToolbar = 1 << 0,
        Tool_SingleWindow = 1 << 1,
        Tool_Singleton =    1 << 2,
    };

    // Bitwise OR operator (to combine flags)
    constexpr EEditorToolFlags operator|(EEditorToolFlags lhs, EEditorToolFlags rhs)
    {
        return static_cast<EEditorToolFlags>(
            static_cast<uint8>(lhs) | static_cast<uint8>(rhs));
    }

    // Bitwise AND operator (to check flags)
    constexpr EEditorToolFlags operator&(EEditorToolFlags lhs, EEditorToolFlags rhs)
    {
        return static_cast<EEditorToolFlags>(
            static_cast<uint8>(lhs) & static_cast<uint8>(rhs));
    }

    // Bitwise XOR operator (to toggle flags)
    constexpr EEditorToolFlags operator^(EEditorToolFlags lhs, EEditorToolFlags rhs)
    {
        return static_cast<EEditorToolFlags>(
            static_cast<uint8>(lhs) ^ static_cast<uint8>(rhs));
    }

    // Bitwise NOT operator (to invert flags)
    constexpr EEditorToolFlags operator~(EEditorToolFlags flag)
    {
        return static_cast<EEditorToolFlags>(~static_cast<uint8>(flag));
    }

    // Compound assignment OR operator (to add a flag)
    constexpr EEditorToolFlags& operator|=(EEditorToolFlags& lhs, EEditorToolFlags rhs)
    {
        lhs = lhs | rhs;
        return lhs;
    }

    // Compound assignment AND operator (to retain only common flags)
    constexpr EEditorToolFlags& operator&=(EEditorToolFlags& lhs, EEditorToolFlags rhs)
    {
        lhs = lhs & rhs;
        return lhs;
    }

    // Compound assignment XOR operator (to toggle flags)
    constexpr EEditorToolFlags& operator^=(EEditorToolFlags& lhs, EEditorToolFlags rhs)
    {
        lhs = lhs ^ rhs;
        return lhs;
    }

    // Utility function to check if a flag is set
    constexpr bool HasFlag(EEditorToolFlags value, EEditorToolFlags flag)
    {
        return (value & flag) == flag;
    }
}
