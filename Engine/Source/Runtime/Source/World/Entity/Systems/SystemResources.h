#pragma once

// Virtual "resources" a system can declare it touches, modeled as types in the same access graph as
// real components (see FSystemAccess). Declaring one makes a system conflict with every other system
// that touches it, so non-thread-safe shared state serializes correctly.
namespace Lumina::SystemResource
{
    struct EventDispatcher {};  // dispatches world events (the dispatcher is not thread-safe)
    struct EntityStructure {};  // does structural ECS changes (create/destroy/add/remove component)
    struct PhysicsQuery {};     // issues physics queries against the live scene
    struct Input {};            // mutates shared input state (layer stack, mouse/input mode)
}

namespace Lumina
{
    // Resources are not components: they never get a registry pool, so the scheduler must not try to
    // pre-create one for them (it would leave a permanent empty pool that no entity can ever join).
    template<typename T>
    inline constexpr bool TIsSystemResource = false;

    template<> inline constexpr bool TIsSystemResource<SystemResource::EventDispatcher> = true;
    template<> inline constexpr bool TIsSystemResource<SystemResource::EntityStructure> = true;
    template<> inline constexpr bool TIsSystemResource<SystemResource::PhysicsQuery>    = true;
    template<> inline constexpr bool TIsSystemResource<SystemResource::Input>           = true;
}
