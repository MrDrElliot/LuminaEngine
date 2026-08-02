#pragma once

#include "Platform/GenericPlatform.h"
#include "Containers/Array.h"
#include "Core/Object/ObjectMacros.h"
#include "World/Entity/EntityHandle.h"
#include "NetGUID.generated.h"

namespace Lumina
{
    // Stable, peer-agnostic network identity for an entity. 0 == invalid.
    REFLECT()
    struct RUNTIME_API FNetGUID
    {
        GENERATED_BODY()

        PROPERTY(ReadOnly, NoSerialize)
        uint32 Value = 0;

        constexpr FNetGUID() = default;
        constexpr explicit FNetGUID(uint32 InValue) : Value(InValue) {}

        constexpr bool IsValid() const { return Value != 0; }
        constexpr bool operator==(const FNetGUID& Other) const { return Value == Other.Value; }
        constexpr bool operator!=(const FNetGUID& Other) const { return Value != Other.Value; }

        static constexpr FNetGUID Invalid() { return FNetGUID{}; }
    };

    // Stable (pre-placed, deterministically assigned) NetGUIDs occupy [1, NetGUID_DynamicStart);
    // dynamic (runtime, server-spawned) NetGUIDs start at NetGUID_DynamicStart.
    inline constexpr uint32 NetGUID_DynamicStart = 0x80000000u;

    // Per-world NetGUID <-> entity map. Stable ids are derived identically on every peer from the
    // shared loaded world (deterministic deserialization); dynamic ids are server-allocated and
    // replicated via spawn records (later phase).
    struct FNetGUIDTable
    {
        THashMap<uint32, FEntity> GuidToEntity;
        uint32                    NextDynamic = NetGUID_DynamicStart;

        void Register(FNetGUID Guid, FEntity Entity)
        {
            if (Guid.IsValid())
            {
                GuidToEntity[Guid.Value] = Entity;
            }
        }

        void Unregister(FNetGUID Guid)
        {
            if (Guid.IsValid())
            {
                GuidToEntity.erase(Guid.Value);
            }
        }

        NODISCARD FEntity Find(FNetGUID Guid) const
        {
            auto It = GuidToEntity.find(Guid.Value);
            return It != GuidToEntity.end() ? It->second : entt::null;
        }

        NODISCARD FNetGUID AllocateDynamic() { return FNetGUID{ NextDynamic++ }; }
    };
}
