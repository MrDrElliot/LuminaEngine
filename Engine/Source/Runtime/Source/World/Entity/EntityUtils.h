#pragma once

#include "World/ECS/Registry.h"

#include "Containers/Invoke.h"
#include "Containers/FunctionRef.h"
#include <atomic>
#include "Components/Component.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Core/Math/TransformFwd.h"
#include "Core/Serialization/Archiver.h"
#include "TaskSystem/TaskSystem.h"


namespace Lumina
{
	class CObject;
	class CStruct;
}

namespace Lumina::ECS::Utils
{
	//-------------------------------------------------------------------------
	// Serialization
	//-------------------------------------------------------------------------

	// Parent links read before every entity of a batch exists, attached together once they all do.
	struct FParentLinks
	{
		struct FLink
		{
			ECS::FEntity Child;
			ECS::FEntity Parent;
			uint32       SiblingIndex = 0;
		};

		TVector<FLink> Links;

		// Each legacy child's next sibling, the only order a file older than REGISTRY_PARENT_LINKS kept. Goes with that format.
		THashMap<ECS::FEntity, ECS::FEntity> LegacyNextSibling;

		// Attaches every link whose parent is live, siblings in their saved order.
		RUNTIME_API void Apply(ECS::FRegistry& Registry);
	};

	// Without Links, a loaded parent link is attached at once, so its parent has to exist already.
	RUNTIME_API bool SerializeEntity(FArchive& Ar, ECS::FRegistry& Registry, ECS::FEntity& Entity, FParentLinks* Links = nullptr);
	RUNTIME_API bool SerializeRegistry(FArchive& Ar, ECS::FRegistry& Registry);

	//-------------------------------------------------------------------------
	// Hierarchy
	//-------------------------------------------------------------------------

	// Queries and plain links live on Registry.GetHierarchy() and Registry.AttachChild; these also keep the world transform.
	RUNTIME_API void ReparentEntity(ECS::FRegistry& Registry, ECS::FEntity Child, ECS::FEntity Parent, bool bPreserveWorld = true);
	RUNTIME_API void RemoveFromParent(ECS::FRegistry& Registry, ECS::FEntity Child);

	//-------------------------------------------------------------------------
	// Lifetime
	//-------------------------------------------------------------------------

	RUNTIME_API void DestroyEntity(ECS::FRegistry& Registry, ECS::FEntity Entity);
	RUNTIME_API void DestroyEntityHierarchy(ECS::FRegistry& Registry, ECS::FEntity Entity);
	RUNTIME_API void DetachImmediateChildren(ECS::FRegistry& Registry, ECS::FEntity Entity);

	//-------------------------------------------------------------------------
	// Transform resolve
	//-------------------------------------------------------------------------

	// The current world transform composed from the nearest ancestor that has not moved, writing nothing, so any thread may call it.
	RUNTIME_API FTransform ComputeWorldTransform(const ECS::FRegistry& Registry, ECS::FEntity Entity);

	// The frame's propagation, which drains the dirty queue so the cost follows what moved, and fans out across workers for large sets.
	RUNTIME_API void ResolveAllDirtyTransforms(ECS::FRegistry& Registry);

	// True if any hierarchical transform moved since the last propagation, one relaxed load, so a propagation point can skip when nothing did.
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
	// Takes a flat entity's move as it happens, while its transform is still hot, for a consumer that would otherwise re-read every mover after the drain.
	struct FFlatMoveWriter
	{
		// False hands the move to the consumer's moved list as usual.
		bool (*Write)(void* Context, ECS::FEntity Entity, const FTransform& World) = nullptr;
		void*  Context  = nullptr;

		// The moved list this writer stands in for, which a taken move then skips.
		uint32 Consumer = Constants::kIndexNoneU32;
	};

	struct FTransformDirtyGate
	{
		std::atomic<bool> bAnyDirty{ true };

		// Set and cleared only between frames, while no system writes transforms.
		std::atomic<const FFlatMoveWriter*> FlatMoveWriter{ nullptr };

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

	// The exact test the resolve uses for its flat path, so STransformComponent::bIsFlat never disagrees with it.
	RUNTIME_API bool IsEntityTransformFlat(ECS::FRegistry& Registry, ECS::FEntity Entity);

	// A flat setter already made world equal local, so this only records the move for the render sync and queues a bodied entity's physics re-sync, without raising the dirty signal.
	// A move the gate's writer took skips that writer's consumer.
	RUNTIME_API void PublishFlatMove(FTransformDirtyGate* Gate, ECS::FEntity Entity, bool bPublish, bool bQueueBody, bool bTaken);

	// Offers a flat move to the gate's writer, reporting whether it took it.
	FORCEINLINE bool TakeFlatMove(const FTransformDirtyGate* Gate, ECS::FEntity Entity, const FTransform& World)
	{
		const FFlatMoveWriter* Writer = Gate != nullptr ? Gate->FlatMoveWriter.load(std::memory_order_acquire) : nullptr;
		return Writer != nullptr && Writer->Write(Writer->Context, Entity, World);
	}

	// Installs a writer for the registry's flat moves, false when another holds the gate. Only while no system writes transforms.
	RUNTIME_API bool SetFlatMoveWriter(ECS::FRegistry& Registry, const FFlatMoveWriter* Writer);

	// Removes the writer if it is still the installed one.
	RUNTIME_API void ClearFlatMoveWriter(ECS::FRegistry& Registry, const FFlatMoveWriter* Writer);

	// Enqueue an entity whose local transform changed (lock-free, any thread); raises the dirty signal.
	// The two queues are independent, each deduped by its own guard on the caller (bWorldDirty /
	// bBodyDirtyQueued): bQueueTransform feeds the resolve, bQueueBody feeds the physics body re-sync.
	// Bodiless entities pass bQueueBody=false to skip that queue (and its drain) entirely.
	RUNTIME_API void QueueDirtyTransform(FTransformDirtyGate* Gate, ECS::FEntity Entity, bool bQueueTransform, bool bQueueBody);


	// Start recording which entities the resolve moved for one more consumer, such as the render scene or navigation, each draining its own copy.
	// A filtered consumer records only the entities it marks with SetMovedTransformInterest, so a world of moving props costs it nothing.
	RUNTIME_API uint32 AddMovedTransformConsumer(ECS::FRegistry& Registry, bool bFiltered = false);

	// Whether a filtered consumer records this entity's moves. Only while no system writes transforms, since publishers read it unlocked.
	RUNTIME_API void SetMovedTransformInterest(ECS::FRegistry& Registry, uint32 Consumer, ECS::FEntity Entity, bool bInterested);
	RUNTIME_API void RemoveMovedTransformConsumer(ECS::FRegistry& Registry, uint32 Consumer);

	// Appends every entity moved since this consumer last drained. Game thread. False when none.
	RUNTIME_API bool DrainMovedTransforms(ECS::FRegistry& Registry, uint32 Consumer, TVector<ECS::FEntity>& Out);

	// Reports an entity whose drawn pose changed while its transform did not.
	RUNTIME_API void PublishMovedTransform(ECS::FRegistry& Registry, ECS::FEntity Entity);

	// Tag the entity's body (if any) for the physics sync. Single-threaded; for external (non-setter) paths.
	RUNTIME_API void MarkPhysicsBodyDirtyIfBodied(ECS::FRegistry& Registry, ECS::FEntity Entity);

	// Drain the lock-free body-dirty queue (setter-moved entities) and tag the bodied ones with
	// FNeedsPhysicsBodyUpdate. Call once at the physics boundary, just before the body sync consumes the tags.
	RUNTIME_API void FlushDirtyPhysicsBodies(ECS::FRegistry& Registry);

	// External (non-setter) dirtying; the bridge hook converts the tag to the component flag. Single-threaded.
	// Per-frame gameplay uses the component setters instead (ParallelFor-safe).
	RUNTIME_API void MarkTransformDirty(ECS::FRegistry& Registry, ECS::FEntity Entity);

	//-------------------------------------------------------------------------
	// Entity transform accessors
	//-------------------------------------------------------------------------

	RUNTIME_API FQuat    GetEntityRotation(ECS::FRegistry& Registry, ECS::FEntity Entity);
	RUNTIME_API FVector3 GetEntityScale(ECS::FRegistry& Registry, ECS::FEntity Entity);
	RUNTIME_API void     SetEntityScale(ECS::FRegistry& Registry, ECS::FEntity Entity, const FVector3& Scale);

	// Set an entity's world transform by converting to the parent-relative local that resolves back to it.
	// The single source of truth for world->local conversion; writing WorldTransform directly is discarded next resolve.
	RUNTIME_API void SetEntityWorldTransform(ECS::FRegistry& Registry, ECS::FEntity Entity, const FTransform& WorldTransform);

	// Rotates in world space against the parent's current world rotation, computed without a lock so parallel writers of disjoint entities may call it.
	RUNTIME_API void SetEntityWorldRotationCached(ECS::FRegistry& Registry, ECS::FEntity Entity, const FQuat& WorldRotation);

	//-------------------------------------------------------------------------
	// Reflection / queries
	//-------------------------------------------------------------------------

	RUNTIME_API bool EntityHasTag(const FName& Tag, ECS::FRegistry& Registry, ECS::FEntity Entity);

	// A tag lives only in its own per-tag storage, so this walks those and never the unnamed one.
	RUNTIME_API void ForEachEntityTag(const ECS::FRegistry& Registry, ECS::FEntity Entity, TFunctionRef<void(const FName&)> Func);

	// The first tag, for callers that treat an entity as carrying one, or None.
	NODISCARD RUNTIME_API FName GetEntityTag(const ECS::FRegistry& Registry, ECS::FEntity Entity);

	// Replaces every tag the entity carries with Tag, or clears them all for None.
	RUNTIME_API void SetEntityTag(ECS::FRegistry& Registry, ECS::FEntity Entity, const FName& Tag);

	RUNTIME_API void ClearEntityTag(ECS::FRegistry& Registry, ECS::FEntity Entity);

	// Moves tags a copy or a replicated spawn left in the unnamed storage into their per-tag storages.
	RUNTIME_API void RebuildTagStorages(ECS::FRegistry& Registry);
	RUNTIME_API bool HasComponent(ECS::FRegistry& Registry, ECS::FEntity Entity, const CStruct* Type);

	// Remaps every reflected entity field through Map, leaving hierarchy links alone. Unmapped ids stay unless bClearUnmapped.
	RUNTIME_API void RemapEntityReferences(ECS::FRegistry& Registry, ECS::FEntity Entity, const THashMap<ECS::FEntity, ECS::FEntity>& Map, bool bClearUnmapped);
	RUNTIME_API void RemapEntityReferences(CObject* Object, const THashMap<ECS::FEntity, ECS::FEntity>& Map, bool bClearUnmapped);
	RUNTIME_API void RemapEntityReferences(const CStruct* Struct, void* Value, const THashMap<ECS::FEntity, ECS::FEntity>& Map, bool bClearUnmapped);

	// Appends every non-null entity RemapEntityReferences would rewrite, duplicates included.
	RUNTIME_API void CollectEntityReferences(ECS::FRegistry& Registry, ECS::FEntity Entity, TVector<ECS::FEntity>& Out);

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

}
