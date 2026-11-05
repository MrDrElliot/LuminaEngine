#pragma once

#include "World/ECS/Registry.h"

#include "Platform/GenericPlatform.h"
#include "Containers/HashTable.h"
#include "Core/Object/ObjectMacros.h"
#include "NetGUID.generated.h"

namespace Lumina
{
    // Stable, peer-agnostic network identity for an entity. 0 == invalid.
    REFLECT()
    struct RUNTIME_API SNetGUID
    {
        GENERATED_BODY()

        PROPERTY(ReadOnly, NoSerialize)
        uint32 Value = 0;

        constexpr SNetGUID() = default;
        constexpr explicit SNetGUID(uint32 InValue) : Value(InValue) {}

        constexpr bool IsValid() const { return Value != 0; }
        constexpr bool operator==(const SNetGUID& Other) const { return Value == Other.Value; }
        constexpr bool operator!=(const SNetGUID& Other) const { return Value != Other.Value; }

        static constexpr SNetGUID Invalid() { return SNetGUID{}; }
    };

    // Stable (pre-placed, deterministically assigned) NetGUIDs occupy [1, NetGUID_DynamicStart);
    // dynamic (runtime, server-spawned) NetGUIDs start at NetGUID_DynamicStart.
    inline constexpr uint32 NetGUID_DynamicStart = 0x80000000u;

    // Per-world NetGUID <-> entity map. Stable ids are derived identically on every peer from the
    // shared loaded world (deterministic deserialization); dynamic ids are server-allocated and
    // replicated via spawn records (later phase).
    struct FNetGUIDTable
    {
        THashMap<uint32, ECS::FEntity> GuidToEntity;
        uint32                    NextDynamic = NetGUID_DynamicStart;

        void Register(SNetGUID Guid, ECS::FEntity Entity)
        {
            if (Guid.IsValid())
            {
                GuidToEntity[Guid.Value] = Entity;
            }
        }

        void Unregister(SNetGUID Guid)
        {
            if (Guid.IsValid())
            {
                GuidToEntity.Remove(Guid.Value);
            }
        }

        NODISCARD ECS::FEntity Find(SNetGUID Guid) const
        {
            auto It = GuidToEntity.Find(Guid.Value);
            return It != nullptr ? (*It) : ECS::NullEntity;
        }

        NODISCARD SNetGUID AllocateDynamic() { return SNetGUID{ NextDynamic++ }; }
    };
}
