#pragma once

#include "World/ECS/Registry.h"

#include "Containers/Invoke.h"
#include <atomic>
#include "Components/Component.h"
#include "Components/RelationshipComponent.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Core/Math/TransformFwd.h"
#include "Core/Serialization/Archiver.h"
#include "TaskSystem/TaskSystem.h"


namespace Lumina
{
	class CStruct;
}

namespace Lumina::ECS::Utils
{
	//-------------------------------------------------------------------------
	// Serialization
	//-------------------------------------------------------------------------

	RUNTIME_API bool SerializeEntity(FArchive& Ar, ECS::FRegistry& Registry, ECS::FEntity& Entity);
	RUNTIME_API bool SerializeRegistry(FArchive& Ar, ECS::FRegistry& Registry);

	//-------------------------------------------------------------------------
	// Hierarchy
	//-------------------------------------------------------------------------

	RUNTIME_API void ReparentEntity(ECS::FRegistry& Registry, ECS::FEntity Child, ECS::FEntity Parent, bool bPreserveWorld = true);
	RUNTIME_API void AddToParent(ECS::FRegistry& Registry, ECS::FEntity Child, ECS::FEntity Parent);
	RUNTIME_API void RemoveFromParent(ECS::FRegistry& Registry, ECS::FEntity Child);
	RUNTIME_API bool IsDescendantOf(ECS::FRegistry& Registry, ECS::FEntity Potential, ECS::FEntity Ancestor);
	RUNTIME_API bool IsChild(ECS::FRegistry& Registry, ECS::FEntity Entity);
	RUNTIME_API bool IsParent(ECS::FRegistry& Registry, ECS::FEntity Entity);

	// Walk up the parent chain to the topmost ancestor; returns Entity itself when it has no parent.
	RUNTIME_API ECS::FEntity GetRootEntity(ECS::FRegistry& Registry, ECS::FEntity Entity);

	RUNTIME_API size_t GetChildCount(ECS::FRegistry& Registry, ECS::FEntity Parent);
	RUNTIME_API void CollectChildren(ECS::FRegistry& Registry, ECS::FEntity Entity, TVector<ECS::FEntity>& OutChildren);
	RUNTIME_API void CollectDescendants(ECS::FRegistry& Registry, ECS::FEntity Entity, TVector<ECS::FEntity>& OutDescendants);

	//-------------------------------------------------------------------------
	// Lifetime
	//-------------------------------------------------------------------------

	RUNTIME_API void DestroyEntity(ECS::FRegistry& Registry, ECS::FEntity Entity);
	RUNTIME_API void DestroyEntityHierarchy(ECS::FRegistry& Registry, ECS::FEntity Entity);
	RUNTIME_API void DetachImmediateChildren(ECS::FRegistry& Registry, ECS::FEntity Entity);

	//-------------------------------------------------------------------------
	// Transform resolve
	//-------------------------------------------------------------------------

	RUNTIME_API void ResolveTransformChain(ECS::FRegistry& Registry, ECS::FEntity Entity);

	// Resolve every dirty transform + descendants by draining the lock-free dirty queue (O(dirty), not a
	// scan). Serialized by the state's resolve guard so concurrent boundary/lazy resolves never write the
	// same WorldTransform; the per-entity work fans out via Task::ParallelFor for large sets.
	RUNTIME_API void ResolveAllDirtyTransforms(ECS::FRegistry& Registry);

	// True if any transform has been dirtied since the last resolve (one relaxed atomic load). Lets the
	// scheduler skip the resolve barrier when nothing moved. After ResolveAllDirtyTransforms this is false,
	// so subsequent GetWorld* reads take the guard-free fast path (pure cached reads => safe in parallel).
	RUNTIME_API bool AnyTransformsDirty(ECS::FRegistry& Registry);

	/**
	 * The one piece of the dirty state that has to be readable INLINE: the gate every world read checks
	 * to decide whether a resolve is owed.
	 *
	 * It lives in the header, and FTransformDirtyState derives from it, so STransformComponent can answer
	 * "is anything dirty?" with a load through a pointer it already cached at Bind. Reading it through the
	 * opaque state instead meant every GetWorld* re-derived the state from the registry context first --
	 * a type-keyed hash lookup, per call, to discover that nothing had moved.
	 */
	struct FTransformDirtyGate
	{
		std::atomic<bool> bAnyDirty{ true };

		/**
		 * Bumped once per drain of the moved channel (i.e. once a frame). A flat setter publishes only when
		 * its component's stamp differs, so an entity written several times in a frame -- SetLocation then
		 * SetRotation then SetScale is the ordinary case -- enqueues once instead of once per call.
		 *
		 * A stamp rather than a flag because a flag would need clearing, and the only place to clear it is a
		 * pass over the drained entities: one random-access component lookup each, which costs more for the
		 * write-once entities than the duplicates it saves. Starts at 1 so a zero-initialized component never
		 * matches on its first write.
		 */
		std::atomic<uint32> PublishEpoch{ 1 };
	};

	// Opaque per-registry transform dirty state (lock-free dirty queues + resolve guard). Lives in the
	// registry context; cached on each STransformComponent at Bind so setters enqueue without a lookup.
	// Derives from FTransformDirtyGate, single and non-virtual, so the two pointers are interchangeable.
	struct FTransformDirtyState;
	RUNTIME_API FTransformDirtyState* EnsureTransformDirtyState(ECS::FRegistry& Registry);

	// The same object seen as its gate. Exists because a caller that only has the forward declaration
	// cannot perform the derived-to-base conversion itself; this is what components cache.
	RUNTIME_API FTransformDirtyGate* EnsureTransformDirtyGate(ECS::FRegistry& Registry);

	// True when the entity carries no FRelationshipComponent. This is the EXACT test the resolve uses to
	// take its flat path, so STransformComponent::bIsFlat (which caches it) can never disagree with the
	// resolver about what flat means.
	RUNTIME_API bool IsEntityTransformFlat(ECS::FRegistry& Registry, ECS::FEntity Entity);

	// A flat entity's setter resolved itself: world == local, written in place while it was still in
	// registers. All that is left is to record the move for the render sync and, for a bodied entity, to
	// queue the physics re-sync. Deliberately does NOT raise the dirty signal -- there is nothing for the
	// resolve to do, and raising it would make the next barrier drain a queue this entity is not in.
	RUNTIME_API void PublishFlatMove(FTransformDirtyGate* Gate, ECS::FEntity Entity, bool bPublish, bool bQueueBody);

	// Enqueue an entity whose local transform changed (lock-free, any thread); raises the dirty signal.
	// The two queues are independent, each deduped by its own guard on the caller (bWorldDirty /
	// bBodyDirtyQueued): bQueueTransform feeds the resolve, bQueueBody feeds the physics body re-sync.
	// Bodiless entities pass bQueueBody=false to skip that queue (and its drain) entirely.
	RUNTIME_API void QueueDirtyTransform(FTransformDirtyGate* Gate, ECS::FEntity Entity, bool bQueueTransform, bool bQueueBody);

	// Start recording which entities the resolve actually moved, so a downstream cache (the render
	// scene's persistent primitive table) can refresh just those instead of rescanning every entity.
	// Off by default: nothing accumulates until a consumer opts in.
	//
	// The resolve is what clears bWorldDirty, so this is the only point where "who moved" is still
	// observable. It reports the resolve's own output rather than duplicating its bookkeeping.
	RUNTIME_API void SetPublishMovedTransforms(ECS::FRegistry& Registry, bool bEnable);

	// Appends every entity moved since the last drain. Game thread. False when none.
	RUNTIME_API bool DrainMovedTransforms(ECS::FRegistry& Registry, TVector<ECS::FEntity>& Out);

	// Tag the entity's body (if any) for the physics sync. Single-threaded; for external (non-setter) paths.
	RUNTIME_API void MarkPhysicsBodyDirtyIfBodied(ECS::FRegistry& Registry, ECS::FEntity Entity);

	// Drain the lock-free body-dirty queue (setter-moved entities) and tag the bodied ones with
	// FNeedsPhysicsBodyUpdate. Call once at the physics boundary, just before the body sync consumes the tags.
	RUNTIME_API void FlushDirtyPhysicsBodies(ECS::FRegistry& Registry);

	// External (non-setter) dirtying; the bridge hook converts the tag to the component flag. Single-threaded.
	// Per-frame gameplay uses the component setters instead (ParallelFor-safe).
	RUNTIME_API void MarkTransformDirty(ECS::FRegistry& Registry, ECS::FEntity Entity);

	// Transform-only dirty: queues the resolve, never the physics body re-sync. For the physics writeback,
	// whose pose came out of the body -- the ordinary paths would queue every simulated body for a teleport
	// back onto its own interpolated (one-step-stale) transform, which drags the simulation backwards.
	RUNTIME_API void MarkTransformDirtyNoBody(ECS::FRegistry& Registry, ECS::FEntity Entity);

	//-------------------------------------------------------------------------
	// Entity transform accessors
	//-------------------------------------------------------------------------

	RUNTIME_API FQuat    GetEntityRotation(ECS::FRegistry& Registry, ECS::FEntity Entity);
	RUNTIME_API FVector3 GetEntityScale(ECS::FRegistry& Registry, ECS::FEntity Entity);
	RUNTIME_API void     SetEntityScale(ECS::FRegistry& Registry, ECS::FEntity Entity, const FVector3& Scale);

	// Set an entity's world transform by converting to the parent-relative local that resolves back to it.
	// The single source of truth for world->local conversion; writing WorldTransform directly is discarded next resolve.
	RUNTIME_API void SetEntityWorldTransform(ECS::FRegistry& Registry, ECS::FEntity Entity, const FTransform& WorldTransform);

	//-------------------------------------------------------------------------
	// Reflection / queries
	//-------------------------------------------------------------------------

	RUNTIME_API bool EntityHasTag(const FName& Tag, ECS::FRegistry& Registry, ECS::FEntity Entity);
	RUNTIME_API bool HasComponent(ECS::FRegistry& Registry, ECS::FEntity Entity, const CStruct* Type);

	// Remap every reflected "Entity"-tagged uint32 field (nested structs too) through Map. FRelationshipComponent
	// links are NOT touched here. Unmapped ids stay put when bClearUnmapped is false, else reset to null.
	RUNTIME_API void RemapEntityReferences(ECS::FRegistry& Registry, ECS::FEntity Entity, const THashMap<ECS::FEntity, ECS::FEntity>& Map, bool bClearUnmapped);

	template<typename TFunc>
	requires(std::is_invocable_v<TFunc, void*, ECS::FSparseSet&, CStruct*>)
	void ForEachComponent(ECS::FRegistry& Registry, ECS::FEntity Entity, TFunc&& Func)
	{
		for (Lumina::ECS::FSparseSet* StoragePtr : Registry.GetActiveStorages())
		{
		    const Lumina::ECS::FComponentTypeID ID = StoragePtr->GetTypeInfo().TypeID;
		    Lumina::ECS::FSparseSet& Storage = *StoragePtr;
			if (Storage.Contains(Entity))
			{
				if (CStruct* Struct = FindComponentStructByTypeId(ID))
				{
					Invoke(Func, Storage.GetRaw(Entity), Storage, Struct);
				}
			}
		}
	}

	template<typename TFunc>
	void ForEachChild(ECS::FRegistry& Registry, ECS::FEntity Parent, TFunc&& Func)
	{
		FRelationshipComponent* ParentRelationship = Registry.TryGet<FRelationshipComponent>(Parent);
		if (!ParentRelationship || ParentRelationship->First == ECS::NullEntity)
		{
			return;
		}

		ECS::FEntity Current = ParentRelationship->First;
		while (Current != ECS::NullEntity)
		{
			ECS::FEntity Next = ECS::NullEntity;
			if (FRelationshipComponent* CurrentRelationship = Registry.TryGet<FRelationshipComponent>(Current))
			{
				Next = CurrentRelationship->Next;
			}

			Invoke(Func, Current);

			Current = Next;
		}
	}

	template<typename TFunc>
	void ForEachDescendant(ECS::FRegistry& Registry, ECS::FEntity Parent, TFunc&& Func)
	{
		ForEachChild(Registry, Parent, [&](ECS::FEntity Child)
		{
			Invoke(Func, Child);
			ForEachDescendant(Registry, Child, Func);
		});
	}

}
