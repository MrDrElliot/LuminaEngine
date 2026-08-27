#pragma once

#include "Containers/HashPrimitives.h"
#include "Platform/GenericPlatform.h"

#include <type_traits>

namespace Lumina::ECS
{
    // A generational handle into one FRegistry, packed into 32 bits so it stays blittable for script and shaders.
    struct FEntity
    {
        static constexpr uint32 IndexBits   = 20;
        static constexpr uint32 VersionBits = 32 - IndexBits;

        static constexpr uint32 IndexMask   = (1u << IndexBits) - 1u;
        static constexpr uint32 VersionMask = (1u << VersionBits) - 1u;

        // One slot index is spent on the null handle, so this is the highest addressable slot.
        static constexpr uint32 MaxIndex = IndexMask - 1u;

        // Reserved for retired dense slots, so no live handle ever carries it.
        static constexpr uint32 TombstoneVersion = VersionMask;

        uint32 Value = ~0u;

        constexpr FEntity() = default;

        constexpr FEntity(uint32 InIndex, uint32 InVersion)
            : Value((InIndex & IndexMask) | ((InVersion & VersionMask) << IndexBits))
        {}

        explicit constexpr FEntity(uint32 Packed) : Value(Packed) {}

        static constexpr FEntity FromPacked(uint32 Packed)
        {
            FEntity Result;
            Result.Value = Packed;
            return Result;
        }

        NODISCARD constexpr uint32 GetIndex() const   { return Value & IndexMask; }
        NODISCARD constexpr uint32 GetVersion() const { return Value >> IndexBits; }
        NODISCARD constexpr uint32 GetPacked() const  { return Value; }

        NODISCARD constexpr bool IsNull() const      { return GetIndex() == IndexMask; }
        NODISCARD constexpr bool IsTombstone() const { return GetVersion() == TombstoneVersion; }

        NODISCARD constexpr FEntity WithVersion(uint32 InVersion) const { return FEntity(GetIndex(), InVersion); }

        // Skips the reserved tombstone version so a recycled slot stays addressable.
        NODISCARD constexpr uint32 GetNextVersion() const
        {
            const uint32 Next = (GetVersion() + 1u) & VersionMask;
            return Next == TombstoneVersion ? 0u : Next;
        }

        constexpr bool operator == (const FEntity& Other) const = default;
        constexpr auto operator <=> (const FEntity& Other) const = default;

        NODISCARD constexpr explicit operator bool() const { return !IsNull(); }

        // The enum this replaced converted with a cast, and call sites still spell it that way.
        NODISCARD constexpr explicit operator uint32() const { return Value; }
    };

    // Serializes as the packed handle. Templated so this header needs no archive dependency.
    template<typename TArchive>
        requires requires (TArchive& Ar, uint32& Value) { Ar << Value; }
    TArchive& operator << (TArchive& Ar, FEntity& Entity)
    {
        return Ar << Entity.Value;
    }

    inline constexpr FEntity NullEntity = FEntity::FromPacked(~0u);

    // Only the version is meaningful; the index carries the storage's free-list link.
    NODISCARD constexpr FEntity MakeTombstone(uint32 NextFreeDenseIndex)
    {
        return FEntity(NextFreeDenseIndex, FEntity::TombstoneVersion);
    }

    // Avalanched, because the table takes its group from the high bits and a raw handle clusters 128 deep.
    NODISCARD constexpr uint64 GetTypeHash(FEntity Entity)
    {
        return Containers::MixHash64(Entity.GetPacked());
    }
}

static_assert(sizeof(Lumina::ECS::FEntity) == 4, "FEntity must stay blittable to a uint32 for script and shaders.");
static_assert(std::is_trivially_copyable_v<Lumina::ECS::FEntity>, "FEntity is passed and stored by value everywhere.");
static_assert(Lumina::ECS::NullEntity.IsNull(), "the null handle must read as null");
static_assert(!Lumina::ECS::FEntity(0, 0).IsNull(), "slot zero at version zero is a real entity");
static_assert(Lumina::ECS::FEntity(7, 3).GetIndex() == 7 && Lumina::ECS::FEntity(7, 3).GetVersion() == 3,
    "index and version round-trip through the packing");
static_assert(Lumina::ECS::MakeTombstone(9).IsTombstone(), "a tombstone reads as one");
static_assert(Lumina::ECS::FEntity(0, Lumina::ECS::FEntity::VersionMask - 1).GetNextVersion() == 0,
    "the version wraps past the reserved tombstone rather than landing on it");
