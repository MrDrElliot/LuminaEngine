#pragma once
#include <atomic>
#include "Components/RelationshipComponent.h"
#include "Containers/Array.h"
#include "Containers/Name.h"
#include "Core/Serialization/Archiver.h"
#include "Registry/EntityRegistry.h"
#include "TaskSystem/TaskSystem.h"


namespace Lumina
{
	class CStruct;
	struct VTransform;
	using FTransform = VTransform;
}

namespace Lumina::ECS::Utils
{
	//-------------------------------------------------------------------------
	// Serialization
	//-------------------------------------------------------------------------

	RUNTIME_API bool SerializeEntity(FArchive& Ar, FEntityRegistry& Registry, entt::entity& Entity);
	RUNTIME_API bool SerializeRegistry(FArchive& Ar, FEntityRegistry& Registry);

	//-------------------------------------------------------------------------
	// Hierarchy
	//-------------------------------------------------------------------------

	RUNTIME_API void ReparentEntity(FEntityRegistry& Registry, entt::entity Child, entt::entity Parent, bool bPreserveWorld = true);
	RUNTIME_API void AddToParent(FEntityRegistry& Registry, entt::entity Child, entt::entity Parent);
	RUNTIME_API void RemoveFromParent(FEntityRegistry& Registry, entt::entity Child);
	RUNTIME_API bool IsDescendantOf(FEntityRegistry& Registry, entt::entity Potential, entt::entity Ancestor);
	RUNTIME_API bool IsChild(FEntityRegistry& Registry, entt::entity Entity);
	RUNTIME_API bool IsParent(FEntityRegistry& Registry, entt::entity Entity);

	// Walk up the parent chain to the topmost ancestor; returns Entity itself when it has no parent.
	RUNTIME_API entt::entity GetRootEntity(FEntityRegistry& Registry, entt::entity Entity);

	RUNTIME_API size_t GetChildCount(FEntityRegistry& Registry, entt::entity Parent);
	RUNTIME_API void CollectChildren(FEntityRegistry& Registry, entt::entity Entity, TVector<entt::entity>& OutChildren);
	RUNTIME_API void CollectDescendants(FEntityRegistry& Registry, entt::entity Entity, TVector<entt::entity>& OutDescendants);

	//-------------------------------------------------------------------------
	// Lifetime
	//-------------------------------------------------------------------------

	RUNTIME_API void DestroyEntity(FEntityRegistry& Registry, entt::entity Entity);
	RUNTIME_API void DestroyEntityHierarchy(FEntityRegistry& Registry, entt::entity Entity);
	RUNTIME_API void DetachImmediateChildren(FEntityRegistry& Registry, entt::entity Entity);

	//-------------------------------------------------------------------------
	// Transform resolve
	//-------------------------------------------------------------------------

	RUNTIME_API void ResolveTransformChain(FEntityRegistry& Registry, entt::entity Entity);

	// Resolve every dirty transform + descendants by draining the lock-free dirty queue (O(dirty), not a
	// scan). Serialized by the state's resolve guard so concurrent boundary/lazy resolves never write the
	// same WorldTransform; the per-entity work fans out via Task::ParallelFor for large sets.
	RUNTIME_API void ResolveAllDirtyTransforms(FEntityRegistry& Registry);

	// True if any transform has been dirtied since the last resolve (one relaxed atomic load). Lets the
	// scheduler skip the resolve barrier when nothing moved. After ResolveAllDirtyTransforms this is false,
	// so subsequent GetWorld* reads take the guard-free fast path (pure cached reads => safe in parallel).
	RUNTIME_API bool AnyTransformsDirty(FEntityRegistry& Registry);

	// Opaque per-registry transform dirty state (lock-free dirty queues + resolve guard). Lives in the
	// registry context; cached on each STransformComponent at Bind so setters enqueue without a lookup.
	struct FTransformDirtyState;
	RUNTIME_API FTransformDirtyState* EnsureTransformDirtyState(FEntityRegistry& Registry);

	// Enqueue an entity whose local transform changed (lock-free, any thread); raises the dirty signal.
	// Dedup is the caller's bWorldDirty 0->1 guard. bQueueBody also enqueues it for the physics body
	// re-sync; pass false for bodiless entities to skip that queue (and its drain) entirely.
	RUNTIME_API void QueueDirtyTransform(FTransformDirtyState* State, entt::entity Entity, bool bQueueBody);

	// Start recording which entities the resolve actually moved, so a downstream cache (the render
	// scene's persistent primitive table) can refresh just those instead of rescanning every entity.
	// Off by default: nothing accumulates until a consumer opts in.
	//
	// The resolve is what clears bWorldDirty, so this is the only point where "who moved" is still
	// observable. It reports the resolve's own output rather than duplicating its bookkeeping.
	RUNTIME_API void SetPublishMovedTransforms(FEntityRegistry& Registry, bool bEnable);

	// Appends every entity moved since the last drain. Game thread. False when none.
	RUNTIME_API bool DrainMovedTransforms(FEntityRegistry& Registry, TVector<entt::entity>& Out);

	// Tag the entity's body (if any) for the physics sync. Single-threaded; for external (non-setter) paths.
	RUNTIME_API void MarkPhysicsBodyDirtyIfBodied(FEntityRegistry& Registry, entt::entity Entity);

	// Drain the lock-free body-dirty queue (setter-moved entities) and tag the bodied ones with
	// FNeedsPhysicsBodyUpdate. Call once at the physics boundary, just before the body sync consumes the tags.
	RUNTIME_API void FlushDirtyPhysicsBodies(FEntityRegistry& Registry);

	// External (non-setter) dirtying; the bridge hook converts the tag to the component flag. Single-threaded.
	// Per-frame gameplay uses the component setters instead (ParallelFor-safe).
	RUNTIME_API void MarkTransformDirty(FEntityRegistry& Registry, entt::entity Entity);

	//-------------------------------------------------------------------------
	// Entity transform accessors
	//-------------------------------------------------------------------------

	RUNTIME_API FQuat    GetEntityRotation(FEntityRegistry& Registry, entt::entity Entity);
	RUNTIME_API FVector3 GetEntityScale(FEntityRegistry& Registry, entt::entity Entity);
	RUNTIME_API void     SetEntityScale(FEntityRegistry& Registry, entt::entity Entity, const FVector3& Scale);

	// Set an entity's world transform by converting to the parent-relative local that resolves back to it.
	// The single source of truth for world->local conversion; writing WorldTransform directly is discarded next resolve.
	RUNTIME_API void SetEntityWorldTransform(FEntityRegistry& Registry, entt::entity Entity, const FTransform& WorldTransform);

	//-------------------------------------------------------------------------
	// Reflection / queries
	//-------------------------------------------------------------------------

	RUNTIME_API bool EntityHasTag(const FName& Tag, FEntityRegistry& Registry, entt::entity Entity);
	RUNTIME_API bool HasComponent(FEntityRegistry& Registry, entt::entity Entity, entt::meta_type Type);

	// Remap every reflected "Entity"-tagged uint32 field (nested structs too) through Map. FRelationshipComponent
	// links are NOT touched here. Unmapped ids stay put when bClearUnmapped is false, else reset to null.
	RUNTIME_API void RemapEntityReferences(FEntityRegistry& Registry, entt::entity Entity, const THashMap<entt::entity, entt::entity>& Map, bool bClearUnmapped);

	NODISCARD RUNTIME_API entt::id_type GetTypeID(FStringView Name);
	NODISCARD RUNTIME_API entt::id_type GetTypeID(const CStruct* Type);

	//-------------------------------------------------------------------------
	// Templates: meta invoke + component / hierarchy iteration
	//-------------------------------------------------------------------------

	template<typename ... TArgs>
	entt::meta_any InvokeMetaFunc(const entt::meta_type& MetaType, entt::id_type FunctionID, TArgs&&... Args)
	{
		if (!MetaType)
		{
			return entt::meta_any{};
		}

		auto&& F = MetaType.func(FunctionID);
		if (!F)
		{
			return entt::meta_any{};
		}

		return F.invoke({}, eastl::forward<TArgs>(Args)...);
	}

	template<typename ... TArgs>
	auto InvokeMetaFunc(const entt::id_type& TypeID, entt::id_type FunctionID, TArgs&&... Args)
	{
		return InvokeMetaFunc(entt::resolve(TypeID), FunctionID, Forward<TArgs>(Args)...);
	}

	template<typename TFunc>
	requires(eastl::is_invocable_v<TFunc, void*, entt::basic_sparse_set<>&, entt::meta_type>)
	void ForEachComponent(FEntityRegistry& Registry, entt::entity Entity, TFunc&& Func)
	{
		for (auto&& [ID, Storage] : Registry.storage())
		{
			if (Storage.contains(Entity))
			{
				if (entt::meta_type MetaType = entt::resolve(Storage.info()))
				{
					eastl::invoke(Func, Storage.value(Entity), Storage, MetaType);
				}
			}
		}
	}

	template<typename TFunc>
	void ForEachChild(entt::registry& Registry, entt::entity Parent, TFunc&& Func)
	{
		FRelationshipComponent* ParentRelationship = Registry.try_get<FRelationshipComponent>(Parent);
		if (!ParentRelationship || ParentRelationship->First == entt::null)
		{
			return;
		}

		entt::entity Current = ParentRelationship->First;
		while (Current != entt::null)
		{
			entt::entity Next = entt::null;
			if (FRelationshipComponent* CurrentRelationship = Registry.try_get<FRelationshipComponent>(Current))
			{
				Next = CurrentRelationship->Next;
			}

			eastl::invoke(Func, Current);

			Current = Next;
		}
	}

	template<typename TFunc>
	void ForEachDescendant(entt::registry& Registry, entt::entity Parent, TFunc&& Func)
	{
		ForEachChild(Registry, Parent, [&](entt::entity Child)
		{
			eastl::invoke(Func, Child);
			ForEachDescendant(Registry, Child, Func);
		});
	}

	inline FArchive& operator << (FArchive& Ar, entt::entity& Entity)
	{
		entt::id_type UintEntity = entt::to_integral(Entity);
		Ar << UintEntity;
		Entity = static_cast<entt::entity>(UintEntity);
		return Ar;
	}
}
