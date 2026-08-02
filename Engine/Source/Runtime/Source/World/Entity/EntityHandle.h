#pragma once

// Lumina ECS facade; the only place that includes <entt/entt.hpp>. Everywhere else uses these
// aliases so EnTT doesn't leak into call sites and can be swapped without touching 100+ files.

#include <entt/entt.hpp>

namespace Lumina
{
    // Core handle & registry

    // Opaque per-registry entity handle. Cheap value type; pass by value.
    using FEntity = entt::entity;

    // Integral id used by the meta system (hashed names, type ids, etc.).
    using FEntityID = entt::id_type;

    // The ECS world: component storages + views. One per CWorld.
    using FEntityRegistry = entt::registry;

    // Untyped storage iteration handle (registry.storage() values). Avoid in new code
    // outside ECS internals; reach for view<>/group<> instead.
    using FEntitySparseSet = entt::basic_sparse_set<FEntity>;

    // Sentinel for an invalid entity. Compares equal to any unset FEntity.
    inline constexpr auto NullEntity = entt::null;

    // Cast an entity handle to its underlying integer (storage index + version bits).
    template<typename E = FEntity>
    [[nodiscard]] constexpr auto EntityToIntegral(E Handle) noexcept
    {
        return entt::to_integral(Handle);
    }

    // view<A,B>(EntityExclude<C,D>) → entities with A and B but neither C nor D.
    template<typename... Ts>
    inline constexpr auto EntityExclude = entt::exclude<Ts...>;

    // group<A>(EntityGet<B>) → owning-A, observing-B group.
    template<typename... Ts>
    inline constexpr auto EntityGet = entt::get<Ts...>;


    // Meta / reflection

    // Type-erased meta-system value (the "any" of entt::meta).
    using FMetaAny = entt::meta_any;

    // Reflected type descriptor (functions, traits, conversions).
    using FMetaType = entt::meta_type;

    // Per-DLL meta registry. Non-runtime DLLs seed theirs from the runtime context
    // via GetEngineMetaService(); see Core/Engine/EngineMetaContext.h.
    using FMetaCtx = entt::meta_ctx;

    // RTTI-style descriptor returned by registry storage iteration (info()/type_id<T>()).
    using FTypeInfo = entt::type_info;

    // Sparse-set deletion policy passed when creating custom storages.
    using FDeletionPolicy = entt::deletion_policy;

    // Tag used to skip copy-deduction during meta-data binding (data<&P, FAsIs>(...)).
    using FAsIs = entt::as_is_t;

    // The cross-DLL meta locator. Non-runtime DLLs seed their copy via
    // FMetaLocator::reset(GetEngineMetaService()) so they see the runtime's meta state.
    using FMetaLocator = entt::locator<FMetaCtx>;

    // Meta registration handle. Use with GetEngineMetaContext():
    //   FMetaFactory<T>(GetEngineMetaContext()).type(Name).func<...>(Id);
    template<typename T>
    using FMetaFactory = entt::meta_factory<T>;

    // Compile-time hashed string (the underlying type of "_hs" literals).
    using FHashedString = entt::hashed_string;

    // Runtime-built view over registry storages keyed by type-id. Used by the
    // editor for type-id-driven iteration over reflected components.
    using FRuntimeView = entt::runtime_view;

    // Resolve a type by hashed id within the engine meta context. Equivalent to
    // entt::resolve(Id) but kept namespaced so call sites do not need entt::.
    [[nodiscard]] inline FMetaType ResolveMetaType(FEntityID Id) noexcept
    {
        return entt::resolve(Id);
    }

    [[nodiscard]] inline FMetaType ResolveMetaType(const FTypeInfo& Info) noexcept
    {
        return entt::resolve(Info);
    }

    template<typename T>
    [[nodiscard]] inline FMetaType ResolveMetaType() noexcept
    {
        return entt::resolve<T>();
    }

    // Iterable range over every registered meta type. Mirrors entt::resolve() (no args).
    [[nodiscard]] inline auto ResolveAllMetaTypes() noexcept
    {
        return entt::resolve();
    }

    // Compile-time hash of a type's name. Same value entt uses internally for ids.
    template<typename T>
    [[nodiscard]] inline constexpr FEntityID HashType() noexcept
    {
        return entt::type_hash<T>::value();
    }

    // Type-hash traits class (use FTypeHash<T>::value() in constexpr contexts where
    // the value is fed into entt directly, e.g. data<&P, FAsIs>(FTypeHash<T>::value())).
    template<typename T>
    using FTypeHash = entt::type_hash<T>;

    // Stable type-info descriptor for a type. Compared against FSparseSet::info()
    // to identify which concrete type a storage holds.
    template<typename T>
    [[nodiscard]] inline const FTypeInfo& TypeIdOf() noexcept
    {
        return entt::type_id<T>();
    }

    // Wrap a value as a meta_any without copying, emitted as a reference inside
    // the meta call. Mirrors entt::forward_as_meta.
    using entt::forward_as_meta;


    // Events

    // ECS event dispatcher. CWorld owns one for engine-side dispatch; gameplay
    // code uses CWorld::GetEventBus() / Subscribe<T>() rather than this directly.
    using FEventDispatcher = entt::dispatcher;
}

// "_hs" literal (meta-system function/property ids), pulled into Lumina so
// generated code and call sites don't need `using namespace entt::literals;`.
namespace Lumina::Literals
{
    using entt::literals::operator""_hs;
    using entt::literals::operator""_hws;
}
