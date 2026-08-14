#pragma once

#include <type_traits>
#include "Core/Assertions/Assert.h"

namespace Lumina
{
    class FBitFlags
    {
    public:

        constexpr static uint8 MaxFlags = 32;
        constexpr static uint32 GetFlagMask(uint8 Flag) { return (1u << Flag); }

    public:

        constexpr FBitFlags() = default;
        constexpr explicit FBitFlags(uint32 InFlags) : Flags(InFlags) {}

        constexpr uint32 Get() const { return Flags; }
        constexpr void Set(uint32 InFlags) { Flags = InFlags; }
        constexpr operator uint32() const { return Flags; }

        constexpr bool HasNoFlagsSet() const { return Flags == 0; }
        constexpr bool IsAnyFlagSet() const { return Flags != 0; }

        constexpr bool IsFlagSet(uint8 Flag) const
        {
            DEBUG_ASSERT(Flag < MaxFlags);
            return (Flags & GetFlagMask(Flag)) > 0;
        }

        template<typename T>
        requires(eastl::is_enum_v<T>)
        constexpr bool IsFlagSet(T EnumValue) const
        {
            return IsFlagSet((uint8) EnumValue);
        }

        constexpr void SetFlag(uint8 Flag)
        {
            Flags |= GetFlagMask(Flag);
        }

        template<typename T>
        requires(eastl::is_enum_v<T>)
        constexpr void SetFlag(T EnumValue)
        {
            SetFlag((uint8)EnumValue);
        }

        constexpr void SetFlag(uint8 Flag, bool bValue)
        {
            DEBUG_ASSERT(Flag < MaxFlags);
            bValue ? SetFlag(Flag) : ClearFlag(Flag);
        }

        template<typename T>
        requires(eastl::is_enum_v<T>)
        constexpr void SetFlag(T EnumValue, bool bValue)
        {
            SetFlag((uint8)EnumValue, bValue);
        }

        constexpr void SetAllFlags()
        {
            Flags = 0xFFFFFFFF;
        }

        constexpr bool IsFlagCleared(uint8 Flag) const
        {
            DEBUG_ASSERT(Flag < MaxFlags);
            return (Flags & GetFlagMask(Flag)) == 0;
        }

        template<typename T>
        requires(eastl::is_enum_v<T>)
        constexpr bool IsFlagCleared(T EnumValue)
        {
            return IsFlagCleared((uint8)EnumValue);
        }

        constexpr void ClearFlag(uint8 Flag)
        {
            DEBUG_ASSERT(Flag < MaxFlags);
            Flags &= ~GetFlagMask(Flag);
        }

        template<typename T>
        constexpr void ClearFlag(T EnumValue)
        {
            ClearFlag((uint8)EnumValue);
        }

        constexpr void ClearAllFlags()
        {
            Flags = 0;
        }

        constexpr void FlipFlag(uint8 Flag)
        {
            DEBUG_ASSERT(Flag < MaxFlags);
            Flags ^= GetFlagMask(Flag);
        }

        template<typename T>
        requires std::is_enum_v<T>
        constexpr void FlipFlag(T EnumValue)
        {
            FlipFlag((uint8)EnumValue);
        }


        constexpr void FlipAllFlags()
        {
            Flags = ~Flags;
        }

        constexpr FBitFlags& operator | (uint8 Flag)
        {
            DEBUG_ASSERT(Flag < MaxFlags);
            Flags |= GetFlagMask(Flag);
            return *this;
        }

        constexpr FBitFlags& operator & (uint8 Flag)
        {
            DEBUG_ASSERT(Flag < MaxFlags);
            Flags &= GetFlagMask(Flag);
            return *this;
        }
        
        uint32 Flags = 0;
    };
}

namespace Lumina
{
    template<typename T>
    requires(eastl::is_enum_v<T> && sizeof(T) <= sizeof(uint32))
    class TBitFlags : public FBitFlags
    {
    public:

        using FBitFlags::FBitFlags;

        explicit constexpr TBitFlags(T Value) 
            : FBitFlags(static_cast<uint32>(Value))
        {
            DEBUG_ASSERT((uint32)Value < MaxFlags);
        }

        constexpr TBitFlags(uint32 Int)
            : FBitFlags(Int)
        {}

        constexpr TBitFlags(const TBitFlags<T>& InFlags)
            : FBitFlags(InFlags.Flags)
        {}

        template<typename... Args>
        requires(... && eastl::is_convertible_v<Args, T>)
        constexpr TBitFlags(Args&&... args)
        {
            ((Flags |= 1u << (uint8)std::forward<Args>(args)), ...);
        }

        constexpr TBitFlags& operator=(const TBitFlags& RHS) = default;

        constexpr bool IsFlagSet(T Flag) const { return FBitFlags::IsFlagSet((uint8)Flag); }
        constexpr bool IsFlagCleared(T Flag) const { return FBitFlags::IsFlagCleared((uint8)Flag); }
        constexpr void SetFlag(T Flag) { FBitFlags::SetFlag((uint8)Flag); }
        constexpr void SetFlag(T Flag, bool bValue) { FBitFlags::SetFlag((uint8)Flag, bValue); }
        constexpr void FlipFlag(T Flag) { FBitFlags::FlipFlag((uint8)Flag); }
        constexpr void ClearFlag(T Flag) { FBitFlags::ClearFlag((uint8)Flag); }

        template<typename... TArgs>
        constexpr void SetMultipleFlags(TArgs&&... Args)
        {
            ((Flags |= 1u << (uint8)eastl::forward<TArgs>(Args)), ...);
        }

        template<typename... TArgs>
        constexpr bool AreAnyFlagsSet(TArgs&&... Args) const
        {
            uint32 Mask = 0;
            ((Mask |= 1u << (uint8)eastl::forward<Args>(Args)), ...);
            return (Flags & Mask) != 0;
        }

        constexpr TBitFlags& operator | (T Flag)
        {
            DEBUG_ASSERT((uint8) Flag < MaxFlags);
            Flags |= GetFlagMask(Flag);
            return *this;
        }

        constexpr TBitFlags& operator & (T Flag)
        {
            DEBUG_ASSERT((uint8) Flag < MaxFlags);
            Flags &= GetFlagMask(Flag);
            return *this;
        }
        
    };
}
