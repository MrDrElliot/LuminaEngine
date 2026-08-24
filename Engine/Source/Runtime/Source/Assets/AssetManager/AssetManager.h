#pragma once

#include "Containers/HashTable.h"
#include "Containers/String.h"
#include "GUID/GUID.h"
#include "PrimaryAssetId.h"
#include "TaskSystem/FiberSync.h"
#include "TaskSystem/Future.h"
#include "Core/Profiler/AssetLoadTracker.h"


namespace Lumina
{
	class CObject;
	struct FAssetData;

	// Result of an async load.
	using FAssetHandle = TFuture<CObject*>;

	// Loads objects from packages, deduplicating concurrent requests for the same asset. 
	class RUNTIME_API FAssetManager final
	{
	public:
		FAssetManager() = default;
		LE_NO_COPYMOVE(FAssetManager);

		static FAssetManager& Get();

		// Kick off (or join) an async load. Returns the shared handle for this asset.
		FAssetHandle LoadAssetAsync(const FFixedString& PackagePath, const FGuid& RequestedAsset);
		
		// Load now and return the object. Joins an in-flight async load (fiber-aware wait) if one exists,
		// otherwise loads inline on the caller. nullptr on failure.
		CObject*     LoadAssetSynchronous(const FFixedString& PackagePath, const FGuid& RequestedAsset);

		// Block until every in-flight async load has finished. Fiber-aware.
		void FlushAsyncLoading();

		// Finds the asset whose AssetName == Id.Name with EAssetFlags::Primary set; nullptr on miss.
		FAssetData*  ResolvePrimaryAsset(const FPrimaryAssetId& Id) const;

		CObject*     LoadPrimaryAssetSynchronous(const FPrimaryAssetId& Id);
		
		// Returns an invalid handle if the id doesn't resolve to a Primary asset.
		FAssetHandle LoadPrimaryAssetAsync(const FPrimaryAssetId& Id);

		template<typename T>
		TObjectPtr<T> LoadPrimaryAssetSynchronous(const TPrimaryAssetId<T>& Id)
		{
			return TObjectPtr<T>(static_cast<T*>(LoadPrimaryAssetSynchronous(static_cast<const FPrimaryAssetId&>(Id))));
		}

	private:

		FAssetHandle AcquireLoad(const FGuid& GUID, TPromise<CObject*>& OutPromise, bool& bShouldLoad);
		static void  RecordRequest(const FFixedString& Path, double DurationMs, EAssetLoadOutcome Outcome);
		void         PerformLoad(const FFixedString& Path, const FGuid& GUID, TPromise<CObject*> Promise);

	private:

		FFiberMutex                          RequestMutex;
		THashMap<FGuid, FAssetHandle>        InFlight; // asset GUID -> shared load handle, while loading
	};


	// TPrimaryAssetId<T> templated impls, out-of-line here because they need the full FAssetManager.

	template<typename T>
	TObjectPtr<T> TPrimaryAssetId<T>::LoadSynchronous() const
	{
		CObject* Obj = FAssetManager::Get().LoadPrimaryAssetSynchronous(static_cast<const FPrimaryAssetId&>(*this));
		return TObjectPtr<T>(static_cast<T*>(Obj));
	}

	template<typename T>
	void TPrimaryAssetId<T>::LoadAsync(const TFunction<void(T*)>& Callback) const
	{
		FAssetHandle Handle = FAssetManager::Get().LoadPrimaryAssetAsync(static_cast<const FPrimaryAssetId&>(*this));
		if (!Handle.IsValid())
		{
			if (Callback)
			{
				Callback(nullptr);
			}
			return;
		}
		if (Callback)
		{
			Handle.Then([Callback](CObject*& Obj) { Callback(static_cast<T*>(Obj)); });
		}
	}
}
