#pragma once

#include "AssetData.h"
#include "TextAssetTypes.h"
#include "Containers/Span.h"
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
		static size_t operator() (const FAssetDataPtr& Asset) noexcept
		{
			return Hash::GetHash(Asset->AssetGUID);
		}
	};

	struct FAssetDataPtrEqual
	{
		static bool operator()(const FAssetDataPtr& A, const FAssetDataPtr& B) noexcept
		{
			return A->AssetGUID == B->AssetGUID;
		}
	};

	struct FGuidHash
	{
		static size_t operator()(const FGuid& GUID) noexcept
		{
			return Hash::GetHash(GUID);
		}
	};

	struct FAssetDataGuidEqual
	{
		static bool operator()(const FAssetDataPtr& Asset, const FGuid& GUID) noexcept
		{
			return Asset->AssetGUID == GUID;
		}
	};

	using FAssetDataMap = THashSet<FAssetDataPtr, FAssetDataPtrHash, FAssetDataPtrEqual>;

	// Identity record for a loose text asset (.rml/.rcss), kept in a map separate from FAssetData and the cook dependency graph.
	struct FTextAssetData
	{
		FGuid          Guid;
		FFixedString   Path;
		FName          Name;
		ETextAssetKind Kind          = ETextAssetKind::None;
		FName          OwningPlugin;
		int64          SourceMTimeNs = 0;
	};

	using FTextAssetDataPtr = TSharedPtr<const FTextAssetData>;

	struct FTextAssetPtrHash
	{
		static size_t operator()(const FTextAssetDataPtr& A) noexcept { return Hash::GetHash(A->Guid); }
	};
	struct FTextAssetPtrEqual
	{
		static bool operator()(const FTextAssetDataPtr& A, const FTextAssetDataPtr& B) noexcept { return A->Guid == B->Guid; }
	};
	struct FTextAssetGuidEqual
	{
		static bool operator()(const FTextAssetDataPtr& A, const FGuid& G) noexcept { return A->Guid == G; }
	};

	using FTextAssetMap = THashSet<FTextAssetDataPtr, FTextAssetPtrHash, FTextAssetPtrEqual>;


	class RUNTIME_API FAssetRegistry final
	{
		friend struct FAssetRegistryTestAccess;

	public:

		static FAssetRegistry& Get();

		// Incremental via the binary cache (skips files whose mtime and size match); async on a background job.
		void RunInitialDiscovery();
		void OnInitialDiscoveryCompleted();

		void AssetCreated(const CObject* Asset);
		void AssetDeleted(const FGuid& GUID);
		void AssetRenamed(FStringView OldPath, FStringView NewPath);
		void AssetSaved(CObject* Asset);

		FAssetDataPtr GetAssetByGUID(const FGuid& GUID) const;
		FAssetDataPtr GetAssetByPath(FStringView Path) const;

		// One lock for the whole batch, since a folder of thousands looked up one by one mostly waits on the lock.
		void GetAssetsByPath(TSpan<const FStringView> Paths, TSpan<FAssetDataPtr> OutAssets) const;
		TVector<FAssetDataPtr> FindByPredicate(const TFunction<bool(const FAssetData&)>& Predicate) const;

		// --- Text assets (.rml/.rcss): GUID identity sourced from hidden .lmeta sidecars. ---

		// Editor-only; not called in cooked runtime, where the shipped registry already carries the table.
		void RunTextAssetDiscovery();

		// Resolve/mint a single text asset (used on create + first-touch). Returns its stable GUID.
		FGuid EnsureTextAsset(FStringView Path);

		FTextAssetDataPtr GetTextAssetByGUID(const FGuid& GUID) const;
		FTextAssetDataPtr GetTextAssetByPath(FStringView Path) const;
		TVector<FTextAssetDataPtr> GetTextAssetsOfKind(ETextAssetKind Kind) const;

		void TextAssetCreated(FStringView Path);
		void TextAssetRenamed(FStringView OldPath, FStringView NewPath);
		void TextAssetDeleted(FStringView Path);
		// Remap every tracked text file under OldDir to NewDir after a folder move/rename.
		void TextAssetFolderRenamed(FStringView OldDir, FStringView NewDir);

		// Every asset listing GUID in its Dependencies; O(1) avg via a reverse map built lazily after change.
		TVector<FAssetDataPtr> GetReferencersOf(const FGuid& GUID) const;

		// The cooker bundles this pre-baked registry into the PAK so runtime skips the rescan; the editor cache is JSON.
		void WriteToArchive(FArchive& Ar) const;
		bool LoadFromArchive(FArchive& Ar);

		FAssetRegistryUpdatedDelegate& GetOnAssetRegistryUpdated() { return OnAssetRegistryUpdated; }

		// While suspended, mutators only flag a pending broadcast; the final ResumeBroadcasts fires one. Prefer FScopedAssetRegistryBatch.
		void SuspendBroadcasts();
		void ResumeBroadcasts();

		// Names an in-flight import has claimed but not yet saved, used by Tools/Import/ImportPaths.
		bool IsImportPathReserved(FStringView Key) const;

		// Atomically claims Key unless something has already reserved it, returning whether this call won.
		bool TryReserveImportPath(FStringView Key);

		// Safe to call on a Key nothing reserved.
		void ReleaseImportPath(FStringView Key);

		// For a teardown that cannot pair its releases.
		void ReleaseAllImportPaths();

	private:

		// Always called OUTSIDE AssetsMutex so a listener can re-enter the non-recursive registry without deadlock.
		void NotifyRegistryChanged();

		// Fires the delegate on the main thread, hopping there first, since every subscriber touches editor UI state.
		void DispatchRegistryChanged();

		// True when the cached entry came from a file with this exact stamp, so nothing needs reading.
		bool MatchesCachedStamp(FStringView Path, int64 MTimeNs, uint64 FileSize) const;

		// True when the bytes still hash to the cached entry, which then takes the new stamp without a re-extract.
		bool RefreshStampIfContentUnchanged(FStringView Path, uint64 ContentHash, int64 MTimeNs, uint64 FileSize);

		// Full parse: header + ImportTable -> FAssetData incl. Dependencies.
		void ProcessPackagePath(FStringView Path);
		void ProcessPackage(FStringView Path, int64 MTimeNs, uint64 FileSize);

		void ClearAssets();

		// Persistence to <Project>/Intermediates/AssetRegistry.bin; stale entries re-validated on next discovery.
		void SaveCache() const;
		bool LoadCache();

		// Under a held read lock of AssetsMutex and write lock of ReverseMapMutex.
		void RebuildReverseMap();

		// Disabled-plugin content survives because its mount is not a walked root.
		size_t ReapStaleEntries();


		FAssetRegistryUpdatedDelegate	OnAssetRegistryUpdated;

		// Path lookups are hot over a read mostly set, so they index rather than scan every entry.
		void InvalidatePathIndex() const { bPathIndexValid = false; }
		void RebuildPathIndex() const;
		void EnsurePathIndex() const;

		// Under a held write lock of AssetsMutex.
		const FAssetDataPtr* FindByPathLocked(FStringView Path) const;

		// Inserts under the held AssetsMutex, replacing any entry with the same path or GUID and keeping PathIndex valid.
		void InsertAssetLocked(FAssetDataPtr AssetData);

		mutable FSharedMutex			AssetsMutex;
		FAssetDataMap 					Assets;
		mutable THashMap<FString, FAssetDataPtr> PathIndex;
		mutable bool					bPathIndexValid = false;

		// Inserts under the held TextAssetsMutex unless the GUID is already listed.
		void AddTextAssetLocked(FTextAssetDataPtr Data);

		mutable FSharedMutex			TextAssetsMutex;
		FTextAssetMap					TextAssets;
		mutable THashMap<FString, FTextAssetDataPtr> TextPathIndex;
		mutable bool					bTextPathIndexValid = false;

		// Taken only after AssetsMutex when both are held.
		mutable FSharedMutex			ReverseMapMutex;
		THashMap<FGuid, TVector<FGuid>, FGuidHash> ReverseDepMap; // dep -> referrers
		mutable bool					bReverseMapDirty = true;

		std::atomic<int32>				BroadcastSuspendCount { 0 };
		std::atomic<bool>				bBroadcastPending { false };

		// Snapshot from the latest RunInitialDiscovery, consumed by OnInitialDiscoveryCompleted for the reap pass.
		TVector<FFixedString>			LastDiscoveryWalkedRoots;
		TVector<FFixedString>			LastDiscoveryVisitedPaths; // sorted

		// What the last discovery did, for its summary line and to skip rewriting a cache nothing changed.
		uint64							DiscoveryStartCycles = 0;
		double							DiscoveryCacheLoadMs = 0.0;
		double							DiscoveryWalkMs = 0.0;
		bool							bDiscoveryHadCache = false;
		std::atomic<uint32>				DiscoveryUnchanged { 0 };
		std::atomic<uint32>				DiscoveryRehashed { 0 };
		std::atomic<uint32>				DiscoveryExtracted { 0 };

		mutable FMutex					ImportPathReservationMutex;
		THashSet<FFixedString>			ReservedImportPaths;
	};

	// Suspends FAssetRegistry broadcasts for its lifetime so a bulk operation fires one broadcast at the end.
	struct RUNTIME_API FScopedAssetRegistryBatch
	{
		FScopedAssetRegistryBatch();
		~FScopedAssetRegistryBatch();
		FScopedAssetRegistryBatch(const FScopedAssetRegistryBatch&) = delete;
		FScopedAssetRegistryBatch& operator=(const FScopedAssetRegistryBatch&) = delete;
		FScopedAssetRegistryBatch(FScopedAssetRegistryBatch&&) = delete;
		FScopedAssetRegistryBatch& operator=(FScopedAssetRegistryBatch&&) = delete;
	};

	// Flags itself stale on every registry broadcast, which can come from an import worker.
	struct RUNTIME_API FAssetRegistryStaleLatch
	{
		FAssetRegistryStaleLatch();
		~FAssetRegistryStaleLatch();
		FAssetRegistryStaleLatch(const FAssetRegistryStaleLatch&) = delete;
		FAssetRegistryStaleLatch& operator=(const FAssetRegistryStaleLatch&) = delete;
		FAssetRegistryStaleLatch(FAssetRegistryStaleLatch&&) = delete;
		FAssetRegistryStaleLatch& operator=(FAssetRegistryStaleLatch&&) = delete;

		// Consumes the stale flag; also goes stale when Key differs from CachedKey.
		template<typename T>
		bool Refresh(const T& Key, T& CachedKey)
		{
			if (Key != CachedKey)
			{
				CachedKey = Key;
				bStale.store(true, std::memory_order_relaxed);
			}
			return bStale.exchange(false, std::memory_order_relaxed);
		}

	private:

		FDelegateHandle		Handle;
		std::atomic<bool>	bStale { true };
	};

}
