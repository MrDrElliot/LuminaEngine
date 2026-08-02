#pragma once

#include "AssetData.h"
#include "TextAssetTypes.h"
#include "Core/Delegates/Delegate.h"
#include "Core/Threading/Thread.h"
#include "Memory/SmartPtr.h"
#include "GUID/GUID.h"
#include <atomic>


DECLARE_MULTICAST_DELEGATE(FAssetRegistryUpdatedDelegate);

namespace Lumina
{
	struct FObjectExport;
	struct FPackageHeader;
	class CPackage;
	struct FAssetData;
	class CClass;

	struct FAssetDataPtrHash
	{
		size_t operator() (const TUniquePtr<FAssetData>& Asset) const noexcept
		{
			return Hash::GetHash(Asset->AssetGUID);
		}
	};

	struct FAssetDataPtrEqual
	{
		bool operator()(const TUniquePtr<FAssetData>& A, const TUniquePtr<FAssetData>& B) const noexcept
		{
			return A->AssetGUID == B->AssetGUID;
		}
	};

	struct FGuidHash
	{
		size_t operator()(const FGuid& GUID) const noexcept
		{
			return Hash::GetHash(GUID);
		}
	};

	struct FAssetDataGuidEqual
	{
		bool operator()(const TUniquePtr<FAssetData>& Asset, const FGuid& GUID) const noexcept
		{
			return Asset->AssetGUID == GUID;
		}
	};

	using FAssetDataMap = THashSet<TUniquePtr<FAssetData>, FAssetDataPtrHash, FAssetDataPtrEqual>;

	// Lightweight identity record for a loose text asset (.luau/.rml/.rcss). Lives in a map entirely
	// separate from FAssetData / the .lasset cook dependency graph. GUID sourced from the file's sidecar.
	struct FTextAssetData
	{
		FGuid          Guid;
		FFixedString   Path;
		FName          Name;
		ETextAssetKind Kind          = ETextAssetKind::None;
		FName          OwningPlugin;
		int64          SourceMTimeNs = 0;
	};

	struct FTextAssetPtrHash
	{
		size_t operator()(const TUniquePtr<FTextAssetData>& A) const noexcept { return Hash::GetHash(A->Guid); }
	};
	struct FTextAssetPtrEqual
	{
		bool operator()(const TUniquePtr<FTextAssetData>& A, const TUniquePtr<FTextAssetData>& B) const noexcept { return A->Guid == B->Guid; }
	};
	struct FTextAssetGuidEqual
	{
		bool operator()(const TUniquePtr<FTextAssetData>& A, const FGuid& G) const noexcept { return A->Guid == G; }
	};

	using FTextAssetMap = THashSet<TUniquePtr<FTextAssetData>, FTextAssetPtrHash, FTextAssetPtrEqual>;


	class RUNTIME_API FAssetRegistry final
	{
	public:

		static FAssetRegistry& Get();

		// Walks engine/project/plugin content, extracting FAssetData (incl. Dependencies) per .lasset.
		// Incremental via .assetdb cache (mtime + content hash); async on Task::AsyncTask.
		void RunInitialDiscovery();
		void OnInitialDiscoveryCompleted();

		void AssetCreated(const CObject* Asset);
		void AssetDeleted(const FGuid& GUID);
		void AssetRenamed(FStringView OldPath, FStringView NewPath);
		void AssetSaved(CObject* Asset);

		FAssetData* GetAssetByGUID(const FGuid& GUID) const;
		FAssetData* GetAssetByPath(FStringView Path) const;
		TVector<FAssetData*> FindByPredicate(const TFunction<bool(const FAssetData&)>& Predicate);

		// --- Text assets (.luau/.rml/.rcss): GUID identity sourced from hidden .lmeta sidecars. ---

		// Editor-only: walk content roots, read-or-mint a sidecar GUID per text file, rebuild the map.
		// Not called in cooked runtime (the shipped registry already carries the table).
		void RunTextAssetDiscovery();

		// Resolve/mint a single text asset (used on create + first-touch). Returns its stable GUID.
		FGuid EnsureTextAsset(FStringView Path);

		FTextAssetData* GetTextAssetByGUID(const FGuid& GUID) const;
		FTextAssetData* GetTextAssetByPath(FStringView Path) const;
		TVector<FTextAssetData*> GetTextAssetsOfKind(ETextAssetKind Kind) const;

		void TextAssetCreated(FStringView Path);
		void TextAssetRenamed(FStringView OldPath, FStringView NewPath);
		void TextAssetDeleted(FStringView Path);
		// Remap every tracked text file under OldDir to NewDir after a folder move/rename.
		void TextAssetFolderRenamed(FStringView OldDir, FStringView NewDir);

		const FTextAssetMap& GetTextAssets() const { return TextAssets; }

		// Every asset listing GUID in its Dependencies; O(1) avg via a reverse map built lazily after change.
		TVector<FAssetData*> GetReferencersOf(const FGuid& GUID) const;

		// Serialize the live registry to compact binary; the cooker bundles a pre-baked registry into
		// the PAK so the runtime skips the filesystem rescan at start. (The editor cache is JSON instead;
		// see SaveCache/LoadCache.)
		void WriteToArchive(FArchive& Ar) const;
		bool LoadFromArchive(FArchive& Ar);

		FAssetRegistryUpdatedDelegate& GetOnAssetRegistryUpdated() { return OnAssetRegistryUpdated; }

		// Coalesce registry-update broadcasts across a bulk operation (e.g. importing many assets). While
		// suspended (count > 0) the per-asset mutators record a pending broadcast instead of firing one each;
		// the final ResumeBroadcasts fires a single broadcast iff anything changed. Reentrant/thread-safe.
		// Prefer the FScopedAssetRegistryBatch guard below.
		void SuspendBroadcasts();
		void ResumeBroadcasts();

		const FAssetDataMap& GetAssets() const { return Assets; }
		const TVector<FString>& GetFailedAssets() const { return FailedAssets; }

	private:

		// Fire OnAssetRegistryUpdated, or, while broadcasts are suspended, just flag one pending. Always called
		// OUTSIDE AssetsMutex so a listener can re-enter the (non-recursive) registry without self-deadlock.
		void NotifyRegistryChanged();

		// True iff the on-disk asset is new or changed since the cached entry was extracted.
		bool NeedsReextract(FStringView Path, int64 MTimeNs, uint64 ContentHash) const;

		// Full parse: header + ImportTable -> FAssetData incl. Dependencies.
		void ProcessPackagePath(FStringView Path);

		void ClearAssets();

		void BroadcastRegistryUpdate();

		void RecordFailedAsset(FStringView Path);

		// Persistence to <EngineInstall>/Intermediates/AssetRegistry.assetdb; stale entries re-validated on next discovery.
		void SaveCache() const;
		bool LoadCache();

		// (Re)build the reverse-dep map from current Assets. Cheap O(N*avgDeps).
		void RebuildReverseMap();

		// Reap cached entries under a walked root not visited this discovery (externally deleted).
		// Disabled-plugin content survives: its mount isn't a walked root.
		void ReapStaleEntries();


		FAssetRegistryUpdatedDelegate	OnAssetRegistryUpdated;

		mutable FSharedMutex			AssetsMutex;
		FAssetDataMap 					Assets;

		mutable FSharedMutex			TextAssetsMutex;
		FTextAssetMap					TextAssets;

		mutable FSharedMutex			FailedAssetsMutex;
		TVector<FString>				FailedAssets;

		mutable FSharedMutex			ReverseMapMutex;
		THashMap<FGuid, TVector<FGuid>, FGuidHash> ReverseDepMap; // dep -> referrers
		mutable bool					bReverseMapDirty = true;

		// Broadcast coalescing (see SuspendBroadcasts). Suspend count is reentrant; pending flag survives until
		// the final resume fires the single broadcast.
		std::atomic<int32>				BroadcastSuspendCount { 0 };
		std::atomic<bool>				bBroadcastPending { false };

		// Snapshot from the most recent RunInitialDiscovery, consumed by
		// OnInitialDiscoveryCompleted to drive the reap pass.
		TVector<FFixedString>			LastDiscoveryWalkedRoots;
		TVector<FFixedString>			LastDiscoveryVisitedPaths; // sorted
	};

	// RAII: suspends FAssetRegistry broadcasts for its lifetime so a bulk operation (importing many assets)
	// fires one registry-update broadcast at the end instead of one per asset. Reentrant (nests safely).
	struct RUNTIME_API FScopedAssetRegistryBatch
	{
		FScopedAssetRegistryBatch();
		~FScopedAssetRegistryBatch();
		FScopedAssetRegistryBatch(const FScopedAssetRegistryBatch&) = delete;
		FScopedAssetRegistryBatch& operator=(const FScopedAssetRegistryBatch&) = delete;
	};

}
