#pragma once

// Virtual "resources" a system can declare it touches, modeled as types in the same access graph as
// real components (see FSystemAccess). Declaring one makes a system conflict with every other system
// that touches it, so non-thread-safe shared state serializes correctly.
namespace Lumina::SystemResource
{
    struct EventDispatcher {};  // dispatches entt events (the dispatcher is not thread-safe)
    struct EntityStructure {};  // does structural ECS changes (create/destroy/add/remove component)
    struct PhysicsQuery {};     // issues physics queries against the live scene
}
