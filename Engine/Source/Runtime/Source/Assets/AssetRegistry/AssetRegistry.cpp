#include "Platform/Time/PlatformTime.h"
#include "RuntimePCH.h"
#include <string>
#include "AssetRegistry.h"

#include "TextAssetSidecar.h"
#include "Core/Delegates/CoreDelegates.h"
#include "Core/Engine/Engine.h"
#include "Core/Math/Hash/Hash.h"
#include "Core/Object/Package/Package.h"
#include "Core/Plugin/Plugin.h"
#include "Core/Plugin/PluginManager.h"
#include "Core/Serialization/Archiver.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/Package/PackageNameTable.h"
#include "FileSystem/FileSystem.h"
#include "Memory/MemoryTracking.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/FileHelper.h"
#include "TaskSystem/TaskSystem.h"
#include "TaskSystem/ThreadedCallback.h"
#include "Tools/UI/ImGui/ImGuiX.h"


#include "Platform/Filesystem/PlatformFilesystem.h"
#include "Log/Log.h"

namespace Lumina
{
    // Tag for the cooked-runtime registry blob bundled into the PAK.
    static constexpr uint32 kAssetRegistryCacheTag     = 0xA55E1DB3; // 'AssetIDB3'

    // Schema version for the human-readable on-disk editor cache.

    FAssetRegistry& FAssetRegistry::Get()
    {
        static FAssetRegistry Registry;
        return Registry;
    }

    namespace
    {
        // Per project, since one shared cache made every project switch reap and re-extract the other's entries.
        FString AssetDbPath()
        {
            FString Out;
            if (Global::GEngine != nullptr && !Global::GEngine->GetProjectPath().IsEmpty())
            {
                const FStringView Project = Global::GEngine->GetProjectPath();
                Out = FStringView(Project.GetData(), Project.Len());
            }
            else
            {
                Out = Paths::GetEngineInstallDirectory();
            }
            if (Out.IsEmpty()) return {};
            Out.SetLen(Paths::TrimTrailingSeparators(Out).Len());
            Out += "/Intermediates/AssetRegistry.bin";
            return Out;
        }

        struct FDiscoveredPackage
        {
            FFixedString Path;
            int64        MTimeNs = 0;
            uint64       FileSize = 0;
        };

        template<typename TEntries, typename TValue>
        void RebuildPathIndexOf(const TEntries& Entries, THashMap<FString, TValue>& Index, bool& bIndexValid)
        {
            Index.Reset();
            Index.Reserve(Entries.Num());
            for (const auto& Data : Entries)
            {
                Index.TryAdd(FString(VFS::RemoveExtension(Data->Path)), Data);
            }
            bIndexValid = true;
        }

        // The index hashes strings and views alike, so the lookup needs no copy of the key.
        template<typename TEntries, typename TValue>
        TValue FindInPathIndex(FSharedMutex& Mutex, const TEntries& Entries, THashMap<FString, TValue>& Index, bool& bIndexValid, FStringView Path)
        {
            const FStringView Key = VFS::RemoveExtension(Path);
            {
                FReadScopeLock Lock(Mutex);
                if (bIndexValid)
                {
                    auto Found = Index.Find(Key);
                    return Found == nullptr ? TValue() : (*Found);
                }
            }

            FWriteScopeLock Lock(Mutex);
            if (!bIndexValid)
            {
                RebuildPathIndexOf(Entries, Index, bIndexValid);
            }
            auto Found = Index.Find(Key);
            return Found == nullptr ? TValue() : (*Found);
        }

        int64 FileMTimeNanos(FStringView VirtualPath)
        {
            const FPathString Resolved = VFS::ResolvePath(VirtualPath);
            return Resolved.IsEmpty() ? 0 : Filesystem::LastWriteTime(Resolved);
        }

        // Read through the VFS so it works for any mounted alias, including a plugin's content.
        uint64 ContentHashOf(FStringView VirtualPath, TVector<uint8>* OutBytes = nullptr)
        {
            TVector<uint8> Bytes;
            if (!VFS::ReadFile(Bytes, VirtualPath))
            {
                return 0;
            }
            const uint64 H = Hash::XXHash::GetHash64(Bytes.GetData(), Bytes.Num());
            if (OutBytes)
            {
                *OutBytes = Move(Bytes);
            }
            return H;
        }

        // Returns the plugin name without the slash, or empty when the alias is not a plugin.
        FName ExtractOwningPlugin(FStringView VirtualPath)
        {
            if (VirtualPath.IsEmpty() || VirtualPath[0] != '/')
            {
                return FName();
            }
            int32 SecondSlash = VirtualPath.Find('/', 1);
            if (SecondSlash == Constants::kIndexNone)
            {
                return FName();
            }
            FStringView Alias = VirtualPath.Mid(1, SecondSlash - 1);
            if (Alias == "Game" || Alias == "Engine" || Alias == "Config")
            {
                return FName();
            }
            if (FPluginManager::Get().FindPlugin(Alias) != nullptr)
            {
                return FName(Alias);
            }
            return FName();
        }
    }

    void FAssetRegistry::RunInitialDiscovery()
    {
        LUMINA_MEMORY_SCOPE("Asset Registry");
        LUMINA_PROFILE_SCOPE();

        DiscoveryStartCycles = PlatformTime::Cycles();
        DiscoveryUnchanged.store(0, std::memory_order_relaxed);
        DiscoveryRehashed.store(0, std::memory_order_relaxed);
        DiscoveryExtracted.store(0, std::memory_order_relaxed);

        // The discovery pass below only touches entries whose mtime or content changed.
        const bool bHadCache = LoadCache();
        bDiscoveryHadCache = bHadCache;
        if (!bHadCache)
        {
            ClearAssets();
        }
        DiscoveryCacheLoadMs = PlatformTime::ToMilliseconds(PlatformTime::Cycles() - DiscoveryStartCycles);
        const uint64 WalkStart = PlatformTime::Cycles();

        TVector<FDiscoveredPackage> Packages;
        TVector<FFixedString> WalkedRoots;
        Packages.Reserve(256);
        WalkedRoots.Reserve(8);

        auto Callback = [&](const VFS::FFileInfo& File)
        {
            if (File.IsDirectory())
            {
                return;
            }
            if (File.IsLAsset())
            {
                Packages.Emplace(FFixedString(*File.VirtualPath, File.VirtualPath.Len()), File.LastModifyTime, File.Size);
            }
        };

        WalkedRoots.Emplace(FFixedString("/Engine/Resources/Content"));
        VFS::RecursiveDirectoryIterator("/Engine/Resources/Content", Callback);

        WalkedRoots.Emplace(FFixedString("/Game/Content"));
        VFS::RecursiveDirectoryIterator("/Game/Content", Callback);
        
        for (const FPlugin* Plugin : FPluginManager::Get().GetAllPlugins())
        {
            if (!Plugin->IsEnabled())
            {
                continue;
            }
            if (!Plugin->IsContentMounted())
            {
                continue;
            }
            const FString MountAlias = Plugin->GetMountAlias();
            WalkedRoots.Emplace(FFixedString(*MountAlias));
            VFS::RecursiveDirectoryIterator(MountAlias, Callback);
        }

        // Snapshotted so completion can reap cache entries whose files no longer exist under a walked root.
        LastDiscoveryWalkedRoots = WalkedRoots;
        LastDiscoveryVisitedPaths.Reset();
        LastDiscoveryVisitedPaths.Reserve(Packages.Num());
        for (const FDiscoveredPackage& Package : Packages)
        {
            LastDiscoveryVisitedPaths.Add(Package.Path);
        }
        Algo::Sort(LastDiscoveryVisitedPaths);

        RunTextAssetDiscovery();
        DiscoveryWalkMs = PlatformTime::ToMilliseconds(PlatformTime::Cycles() - WalkStart);

        const uint32 NumPackages = (uint32)Packages.Num();
        if (NumPackages == 0)
        {
            OnInitialDiscoveryCompleted();
            return;
        }

        Jobs::Spawn(Jobs::EJobPriority::Background, [](FAssetRegistry* Self, TVector<FDiscoveredPackage> Discovered) -> Jobs::TTask<void>
        {
            for (const FDiscoveredPackage& Package : Discovered)
            {
                Self->ProcessPackage(FStringView(*Package.Path, Package.Path.Len()), Package.MTimeNs, Package.FileSize);
            }

            Self->OnInitialDiscoveryCompleted();
            co_return;
        }, this, Move(Packages));
    }

    void FAssetRegistry::OnInitialDiscoveryCompleted()
    {
        // Dropped before the registry is handed out or persisted.
        const size_t Reaped = ReapStaleEntries();

        const uint32 Unchanged = DiscoveryUnchanged.load(std::memory_order_relaxed);
        const uint32 Rehashed  = DiscoveryRehashed.load(std::memory_order_relaxed);
        const uint32 Extracted = DiscoveryExtracted.load(std::memory_order_relaxed);
        const double ProcessMs = PlatformTime::ToMilliseconds(PlatformTime::Cycles() - DiscoveryStartCycles) - DiscoveryCacheLoadMs - DiscoveryWalkMs;

        // Persist the cache so next launch only re-parses changed assets, unless this pass changed nothing in it.
        const uint64 SaveStart = PlatformTime::Cycles();
        const bool bCacheChanged = !bDiscoveryHadCache || Rehashed > 0 || Extracted > 0 || Reaped > 0;
        if (bCacheChanged)
        {
            SaveCache();
        }
        const double SaveMs = PlatformTime::ToMilliseconds(PlatformTime::Cycles() - SaveStart);

        ImGuiX::Notifications::NotifySuccess("Asset Registry Finished Initial Discovery: Num [{}]", Assets.Num());
        LOG_INFO("Asset Registry Finished Initial Discovery: Num [{}] in {:.0f} ms (cache load {:.0f} ms, walk {:.0f} ms, "
                 "process {:.0f} ms with {} unchanged, {} rehashed, {} extracted, {} reaped, save {:.0f} ms{})",
                 Assets.Num(), PlatformTime::ToMilliseconds(PlatformTime::Cycles() - DiscoveryStartCycles),
                 DiscoveryCacheLoadMs, DiscoveryWalkMs, ProcessMs, Unchanged, Rehashed, Extracted, Reaped, SaveMs,
                 bCacheChanged ? "" : ", skipped");

        // Reverse map gets built lazily on first GetReferencersOf().
        {
            FWriteScopeLock Lock(ReverseMapMutex);
            bReverseMapDirty = true;
        }

        DispatchRegistryChanged();
    }

    void FAssetRegistry::EnsurePathIndex() const
    {
        {
            FReadScopeLock Lock(AssetsMutex);
            if (bPathIndexValid)
            {
                return;
            }
        }
        FWriteScopeLock Lock(AssetsMutex);
        if (!bPathIndexValid)
        {
            RebuildPathIndex();
        }
    }

    bool FAssetRegistry::MatchesCachedStamp(FStringView Path, int64 MTimeNs, uint64 FileSize) const
    {
        // A zero stamp means the backing store could not say, so only the bytes can answer.
        if (MTimeNs == 0 || FileSize == 0)
        {
            return false;
        }

        // A classless entry was cached before its header was read; the UI font resolver keys on the class.
        const FAssetDataPtr Data = GetAssetByPath(Path);
        return Data != nullptr && !Data->AssetClass.IsNone() && Data->SourceMTimeNs == MTimeNs && Data->FileSize == FileSize;
    }

    bool FAssetRegistry::RefreshStampIfContentUnchanged(FStringView Path, uint64 ContentHash, int64 MTimeNs, uint64 FileSize)
    {
        FWriteScopeLock Lock(AssetsMutex);
        const FAssetDataPtr* Found = FindByPathLocked(Path);
        if (Found == nullptr || (*Found)->AssetClass.IsNone() || (*Found)->ContentHash != ContentHash)
        {
            return false;
        }

        auto Restamped = MakeShared<FAssetData>(**Found);
        Restamped->SourceMTimeNs = MTimeNs;
        Restamped->FileSize      = FileSize;
        InsertAssetLocked(Move(Restamped));
        return true;
    }

    void FAssetRegistry::SuspendBroadcasts()
    {
        BroadcastSuspendCount.fetch_add(1, std::memory_order_relaxed);
    }

    void FAssetRegistry::ResumeBroadcasts()
    {
        // Last suspender out fires the single coalesced broadcast iff something flagged a change while suspended.
        if (BroadcastSuspendCount.fetch_sub(1, std::memory_order_seq_cst) == 1 && bBroadcastPending.exchange(false, std::memory_order_seq_cst))
        {
            DispatchRegistryChanged();
        }
    }

    bool FAssetRegistry::IsImportPathReserved(FStringView Key) const
    {
        FScopeLock Lock(ImportPathReservationMutex);
        return ReservedImportPaths.Contains(FFixedString(Key.GetData(), Key.Len()));
    }

    bool FAssetRegistry::TryReserveImportPath(FStringView Key)
    {
        FFixedString Fixed(Key.GetData(), Key.Len());
        FScopeLock Lock(ImportPathReservationMutex);
        if (ReservedImportPaths.Contains(Fixed))
        {
            return false;
        }
        ReservedImportPaths.Add(Fixed);
        return true;
    }

    void FAssetRegistry::ReleaseImportPath(FStringView Key)
    {
        FScopeLock Lock(ImportPathReservationMutex);
        ReservedImportPaths.Remove(FFixedString(Key.GetData(), Key.Len()));
    }

    void FAssetRegistry::ReleaseAllImportPaths()
    {
        FScopeLock Lock(ImportPathReservationMutex);
        ReservedImportPaths.Reset();
    }

    void FAssetRegistry::DispatchRegistryChanged()
    {
        // Asset creation runs on a worker, so broadcasting inline raced the main thread's redraw.
        if (Threading::IsMainThread())
        {
            OnAssetRegistryUpdated.Broadcast();
            return;
        }

        MainThread::Enqueue([]
        {
            FAssetRegistry::Get().OnAssetRegistryUpdated.Broadcast();
        });
    }

    void FAssetRegistry::NotifyRegistryChanged()
    {
        // Flagged before the count is read, so a resume racing this either sees the flag or leaves it for the claim below.
        bBroadcastPending.store(true, std::memory_order_seq_cst);
        if (BroadcastSuspendCount.load(std::memory_order_seq_cst) == 0 && bBroadcastPending.exchange(false, std::memory_order_seq_cst))
        {
            DispatchRegistryChanged();
        }
    }

    FScopedAssetRegistryBatch::FScopedAssetRegistryBatch()
    {
        FAssetRegistry::Get().SuspendBroadcasts();
    }

    FScopedAssetRegistryBatch::~FScopedAssetRegistryBatch()
    {
        FAssetRegistry::Get().ResumeBroadcasts();
    }

    FAssetRegistryStaleLatch::FAssetRegistryStaleLatch()
    {
        Handle = FAssetRegistry::Get().GetOnAssetRegistryUpdated().AddLambda([this]()
        {
            bStale.store(true, std::memory_order_relaxed);
        });
    }

    FAssetRegistryStaleLatch::~FAssetRegistryStaleLatch()
    {
        FAssetRegistry::Get().GetOnAssetRegistryUpdated().Remove(Handle);
    }

    void FAssetRegistry::AssetCreated(const CObject* Asset)
    {
        const FFixedString FilePath = Asset->GetPackage()->GetPackagePath();

        auto AssetData = MakeShared<FAssetData>();
        AssetData->AssetClass    = Asset->GetClass()->GetName();
        AssetData->AssetGUID     = Asset->GetGUID();
        AssetData->AssetName     = Asset->GetName();
        AssetData->Path          = FilePath;
        AssetData->OwningPlugin  = ExtractOwningPlugin(AssetData->Path);

        const bool bOnDisk = VFS::Exists(AssetData->Path);

        {
            FWriteScopeLock Lock(AssetsMutex);
            InsertAssetLocked(Move(AssetData));
        }

        // Every create path saves before announcing, so the file's import table is the dependency list a fresh entry lacks.
        if (bOnDisk)
        {
            ProcessPackagePath(FStringView(*FilePath, FilePath.Len()));
        }

        {
            FWriteScopeLock RLock(ReverseMapMutex);
            bReverseMapDirty = true;
        }

        NotifyRegistryChanged();
    }

    const FAssetDataPtr* FAssetRegistry::FindByPathLocked(FStringView Path) const
    {
        if (!bPathIndexValid)
        {
            RebuildPathIndex();
        }
        return PathIndex.Find(VFS::RemoveExtension(Path));
    }

    void FAssetRegistry::InsertAssetLocked(FAssetDataPtr AssetData)
    {
        // A replace import or a file dropped over an old one leaves a new GUID at a known path, and two entries for one path make lookups pick either.
        FString PathKey(VFS::RemoveExtension(AssetData->Path));
        if (const FAssetDataPtr* Stale = FindByPathLocked(AssetData->Path))
        {
            Assets.Remove(*Stale);
            PathIndex.Remove(PathKey);
        }

        // An external move keeps the GUID, so a stale entry would otherwise leave a dangling old path.
        if (const FAssetDataPtr* SameGuid = Assets.FindAs(AssetData->AssetGUID, FGuidHash(), FAssetDataGuidEqual()))
        {
            const FStringView OldPathKey = VFS::RemoveExtension((*SameGuid)->Path);
            const FAssetDataPtr* OldKey = PathIndex.Find(OldPathKey);
            if (OldKey != nullptr && (*OldKey) == (*SameGuid))
            {
                PathIndex.Remove(OldPathKey);
            }
            Assets.Remove(*SameGuid);
        }

        PathIndex.TryAdd(Move(PathKey), AssetData);
        Assets.TryAdd(Move(AssetData));
    }

    void FAssetRegistry::AssetDeleted(const FGuid& GUID)
    {
        {
            FWriteScopeLock Lock(AssetsMutex);
            InvalidatePathIndex();

            auto It = Assets.FindAs(GUID, FGuidHash(), FAssetDataGuidEqual());
            if (It == nullptr)
            {
                LOG_WARN("AssetRegistry::AssetDeleted: GUID not present in registry; ignoring");
                return;
            }

            Assets.Remove(*It);
        }

        {
            FWriteScopeLock RLock(ReverseMapMutex);
            bReverseMapDirty = true;
        }

        NotifyRegistryChanged();
    }

    void FAssetRegistry::AssetRenamed(FStringView OldPath, FStringView NewPath)
    {
        {
            FWriteScopeLock Lock(AssetsMutex);
            const FAssetDataPtr* Found = FindByPathLocked(OldPath);
            if (Found == nullptr)
            {
                LOG_WARN("AssetRegistry::AssetRenamed: no entry for {}; rename of {} -> {} not reflected in registry until next discovery", OldPath, OldPath, NewPath);
                return;
            }

            // Inserted as a new entry, which also drops any stale one already at NewPath.
            auto Renamed = MakeShared<FAssetData>(**Found);
            Renamed->Path         = NewPath;
            Renamed->AssetName    = VFS::FileName(NewPath, true);
            Renamed->OwningPlugin = ExtractOwningPlugin(NewPath);
            InsertAssetLocked(Move(Renamed));
        }

        NotifyRegistryChanged();
    }

    void FAssetRegistry::AssetSaved(CObject* Asset)
    {
        // The saved package's import table and content hash may have changed, so the reverse map dies.
        FFixedString Path = Asset->GetPackage()->GetPackagePath();
        ProcessPackagePath(Path);

        {
            FWriteScopeLock RLock(ReverseMapMutex);
            bReverseMapDirty = true;
        }

        NotifyRegistryChanged();
    }

    FAssetDataPtr FAssetRegistry::GetAssetByGUID(const FGuid& GUID) const
    {
        FReadScopeLock Lock(AssetsMutex);

        auto It = Assets.FindAs(GUID, FGuidHash(), FAssetDataGuidEqual());
        return It == nullptr ? FAssetDataPtr() : (*It);
    }

    void FAssetRegistry::RebuildPathIndex() const
    {
        RebuildPathIndexOf(Assets, PathIndex, bPathIndexValid);
    }

    FAssetDataPtr FAssetRegistry::GetAssetByPath(FStringView Path) const
    {
        LUMINA_PROFILE_SCOPE();
        return FindInPathIndex(AssetsMutex, Assets, PathIndex, bPathIndexValid, Path);
    }

    void FAssetRegistry::GetAssetsByPath(TSpan<const FStringView> Paths, TSpan<FAssetDataPtr> OutAssets) const
    {
        LUMINA_PROFILE_SCOPE();
        DEBUG_ASSERT(Paths.Num() == OutAssets.Num());

        // The index can be invalidated between the rebuild and the read, so the read retries until it sees a valid one.
        while (true)
        {
            {
                FReadScopeLock Lock(AssetsMutex);
                if (bPathIndexValid)
                {
                    for (int32 Index = 0; Index < Paths.Num(); ++Index)
                    {
                        auto Found = PathIndex.Find(VFS::RemoveExtension(Paths[Index]));
                        OutAssets[Index] = Found == nullptr ? FAssetDataPtr() : (*Found);
                    }
                    return;
                }
            }
            EnsurePathIndex();
        }
    }

    TVector<FAssetDataPtr> FAssetRegistry::FindByPredicate(const TFunction<bool(const FAssetData&)>& Predicate) const
    {
        FReadScopeLock Lock(AssetsMutex);

        TVector<FAssetDataPtr> Datas;
        Datas.Reserve(Assets.Num() / 2);
        for (const FAssetDataPtr& Data : Assets)
        {
            if (Predicate(*Data))
            {
                Datas.Emplace(Data);
            }
        }

        return Datas;
    }

    // --- Text assets -------------------------------------------------------------------------------

    void FAssetRegistry::RunTextAssetDiscovery()
    {
        LUMINA_MEMORY_SCOPE("Asset Registry");
        LUMINA_PROFILE_SCOPE();

        TVector<FFixedString> Roots;
        Roots.Emplace(FFixedString("/Engine/Resources/Content"));
        Roots.Emplace(FFixedString("/Game/Content"));
        for (const FPlugin* Plugin : FPluginManager::Get().GetAllPlugins())
        {
            if (!Plugin->IsEnabled())        continue;
            if (!Plugin->IsContentMounted()) continue;
            Roots.Emplace(FFixedString(*Plugin->GetMountAlias()));
        }

        FTextAssetMap Rebuilt;
        const TextAssetSidecar::FContentRootList SidecarRoots = TextAssetSidecar::GatherContentRoots();

        auto Callback = [&](const VFS::FFileInfo& File)
        {
            if (File.IsDirectory()) return;
            const FStringView Vp(*File.VirtualPath, File.VirtualPath.Len());
            if (TextAssetSidecar::IsSidecarPath(Vp)) return;

            const ETextAssetKind Kind = TextAsset::KindFromPath(Vp);
            if (Kind == ETextAssetKind::None) return;

            const FGuid Guid = TextAssetSidecar::ReadOrMint(Vp, SidecarRoots, Kind);
            if (!Guid.IsValid()) return;

            auto Data = MakeShared<FTextAssetData>();
            Data->Guid          = Guid;
            Data->Path = Vp;
            Data->Name          = VFS::FileName(Vp, true);
            Data->Kind          = Kind;
            Data->OwningPlugin  = ExtractOwningPlugin(Vp);
            Data->SourceMTimeNs = FileMTimeNanos(Vp);

            // The first of two files sharing a stale GUID wins, and the duplicate re-mints next pass.
            if (Rebuilt.FindAs(Guid, FGuidHash(), FTextAssetGuidEqual()) == nullptr)
            {
                Rebuilt.TryAdd(Move(Data));
            }
        };

        for (const FFixedString& Root : Roots)
        {
            VFS::RecursiveDirectoryIterator(FStringView(*Root, Root.Len()), Callback);
        }

        FWriteScopeLock Lock(TextAssetsMutex);
        TextAssets = Move(Rebuilt);
        bTextPathIndexValid = false;
    }

    FGuid FAssetRegistry::EnsureTextAsset(FStringView Path)
    {
        if (FTextAssetDataPtr Existing = GetTextAssetByPath(Path))
        {
            return Existing->Guid;
        }

        const ETextAssetKind Kind = TextAsset::KindFromPath(Path);
        if (Kind == ETextAssetKind::None)
        {
            return FGuid();
        }

        const FGuid Guid = TextAssetSidecar::ReadOrMint(Path, Kind);
        if (!Guid.IsValid())
        {
            return FGuid();
        }

        auto Data = MakeShared<FTextAssetData>();
        Data->Guid          = Guid;
        Data->Path = Path;
        Data->Name          = VFS::FileName(Path, true);
        Data->Kind          = Kind;
        Data->OwningPlugin  = ExtractOwningPlugin(Path);
        Data->SourceMTimeNs = FileMTimeNanos(Path);

        FWriteScopeLock Lock(TextAssetsMutex);
        AddTextAssetLocked(Move(Data));
        return Guid;
    }

    void FAssetRegistry::AddTextAssetLocked(FTextAssetDataPtr Data)
    {
        if (TextAssets.FindAs(Data->Guid, FGuidHash(), FTextAssetGuidEqual()) == nullptr)
        {
            TextAssets.TryAdd(Move(Data));
            bTextPathIndexValid = false;
        }
    }

    FTextAssetDataPtr FAssetRegistry::GetTextAssetByGUID(const FGuid& GUID) const
    {
        FReadScopeLock Lock(TextAssetsMutex);
        auto It = TextAssets.FindAs(GUID, FGuidHash(), FTextAssetGuidEqual());
        return It == nullptr ? FTextAssetDataPtr() : (*It);
    }

    FTextAssetDataPtr FAssetRegistry::GetTextAssetByPath(FStringView Path) const
    {
        return FindInPathIndex(TextAssetsMutex, TextAssets, TextPathIndex, bTextPathIndexValid, Path);
    }

    TVector<FTextAssetDataPtr> FAssetRegistry::GetTextAssetsOfKind(ETextAssetKind Kind) const
    {
        FReadScopeLock Lock(TextAssetsMutex);
        TVector<FTextAssetDataPtr> Out;
        for (const FTextAssetDataPtr& Data : TextAssets)
        {
            if (Kind == ETextAssetKind::None || Data->Kind == Kind)
            {
                Out.Add(Data);
            }
        }
        return Out;
    }

    void FAssetRegistry::TextAssetCreated(FStringView Path)
    {
        EnsureTextAsset(Path);
        NotifyRegistryChanged();
    }

    void FAssetRegistry::TextAssetRenamed(FStringView OldPath, FStringView NewPath)
    {
        // Relocate the sidecar first so the GUID travels with the file.
        TextAssetSidecar::Move(OldPath, NewPath);

        bool bRenamed = false;
        {
            FWriteScopeLock Lock(TextAssetsMutex);

            auto It = Algo::FindIf(TextAssets, [&](const FTextAssetDataPtr& Data)
            {
                return FStringView(*Data->Path, Data->Path.Len()) == OldPath;
            });
            if (It == TextAssets.end())
            {
                return;
            }
            bTextPathIndexValid = false;

            // Listed as a new entry, so a holder of the old one keeps reading the old path.
            auto Renamed = MakeShared<FTextAssetData>(**It);
            Renamed->Path         = NewPath;
            Renamed->Name         = VFS::FileName(NewPath, true);
            Renamed->Kind         = TextAsset::KindFromPath(NewPath);
            Renamed->OwningPlugin = ExtractOwningPlugin(NewPath);
            TextAssets.Remove(It);

            // Drop a stale entry already sitting at NewPath with a different GUID.
            auto Colliding = Algo::FindIf(TextAssets, [&](const FTextAssetDataPtr& Data)
            {
                return FStringView(*Data->Path, Data->Path.Len()) == NewPath;
            });
            if (Colliding != TextAssets.end())
            {
                TextAssets.Remove(Colliding);
            }

            TextAssets.TryAdd(Move(Renamed));
            bRenamed = true;
        }

        if (bRenamed)
        {
            // Outside the lock, since subscribers may read the registry back.
            FCoreDelegates::OnContentFileRenamed.Broadcast(OldPath, NewPath);
            NotifyRegistryChanged();
        }
    }

    void FAssetRegistry::TextAssetDeleted(FStringView Path)
    {
        TextAssetSidecar::Delete(Path);

        {
            FWriteScopeLock Lock(TextAssetsMutex);
            auto It = Algo::FindIf(TextAssets, [&](const FTextAssetDataPtr& Data)
            {
                return FStringView(*Data->Path, Data->Path.Len()) == Path;
            });
            if (It != TextAssets.end())
            {
                TextAssets.Remove(It);
                bTextPathIndexValid = false;
            }
        }

        NotifyRegistryChanged();
    }

    void FAssetRegistry::TextAssetFolderRenamed(FStringView OldDir, FStringView NewDir)
    {
        // Snapshot the affected (old) paths first; mutate sidecars + entries outside the iteration.
        TVector<FFixedString> OldPaths;
        {
            FReadScopeLock Lock(TextAssetsMutex);
            for (const FTextAssetDataPtr& Data : TextAssets)
            {
                const FStringView P(*Data->Path, Data->Path.Len());
                if (VFS::IsUnderDirectory(OldDir, P))
                {
                    OldPaths.Emplace(Data->Path);
                }
            }
        }

        for (const FFixedString& Old : OldPaths)
        {
            const FStringView OldView(*Old, Old.Len());
            // new = NewDir + (Old - OldDir)
            FStringView Tail = OldView.Mid(OldDir.Len());
            FFixedString NewPath(NewDir.GetData(), NewDir.Len());
            NewPath.Append(Tail.GetData(), Tail.Len());
            TextAssetRenamed(OldView, FStringView(*NewPath, NewPath.Len()));
        }
    }

    size_t FAssetRegistry::ReapStaleEntries()
    {
        if (LastDiscoveryWalkedRoots.IsEmpty())
        {
            return 0;
        }

        FWriteScopeLock Lock(AssetsMutex);
        InvalidatePathIndex();

        size_t Reaped = 0;
        for (auto It = Assets.begin(); It != Assets.end(); )
        {
            const FFixedString& Path = (*It)->Path;
            const FStringView PathView(*Path, Path.Len());

            bool bUnderWalkedRoot = false;
            for (const FFixedString& Root : LastDiscoveryWalkedRoots)
            {
                const FStringView RootView(*Root, Root.Len());
                if (VFS::IsUnderDirectory(RootView, PathView))
                {
                    bUnderWalkedRoot = true;
                    break;
                }
            }
            if (!bUnderWalkedRoot)
            {
                ++It;
                continue;
            }

            const bool bVisited = Algo::BinarySearch(
                LastDiscoveryVisitedPaths,
                Path);

            if (!bVisited)
            {
                It = Assets.Remove(It);
                ++Reaped;
            }
            else
            {
                ++It;
            }
        }

        if (Reaped > 0)
        {
            LOG_INFO("AssetRegistry: reaped {} cached entries whose files no longer exist", Reaped);
            FWriteScopeLock RLock(ReverseMapMutex);
            bReverseMapDirty = true;
        }

        LastDiscoveryWalkedRoots.Reset();
        LastDiscoveryVisitedPaths.Reset();
        return Reaped;
    }

    void FAssetRegistry::RebuildReverseMap()
    {
        LUMINA_PROFILE_SCOPE();
        LUMINA_MEMORY_SCOPE("Asset Registry");
        ReverseDepMap.Reset();
        ReverseDepMap.Reserve(Assets.Num());
        for (const FAssetDataPtr& Data : Assets)
        {
            for (const FAssetDependency& Dep : Data->Dependencies)
            {
                ReverseDepMap[Dep.TargetGUID].Add(Data->AssetGUID);
            }
        }
        bReverseMapDirty = false;
    }

    TVector<FAssetDataPtr> FAssetRegistry::GetReferencersOf(const FGuid& GUID) const
    {
        FReadScopeLock ALock(AssetsMutex);

        // The const-cast is fine, since the map cache is mutable state.
        {
            FWriteScopeLock RLock(ReverseMapMutex);
            if (bReverseMapDirty)
            {
                const_cast<FAssetRegistry*>(this)->RebuildReverseMap();
            }
        }

        TVector<FAssetDataPtr> Result;
        {
            FReadScopeLock RLock(ReverseMapMutex);
            auto It = ReverseDepMap.Find(GUID);
            if (It == nullptr) return Result;

            Result.Reserve(It->Num());
            for (const FGuid& ReferrerGuid : (*It))
            {
                auto AIt = Assets.FindAs(ReferrerGuid, FGuidHash(), FAssetDataGuidEqual());
                if (AIt != nullptr)
                {
                    Result.Add(*AIt);
                }
            }
        }
        return Result;
    }

    void FAssetRegistry::ProcessPackagePath(FStringView Path)
    {
        const FPathString Resolved = VFS::ResolvePath(Path);
        const Filesystem::FFileStat Stat = Resolved.IsEmpty() ? Filesystem::FFileStat{} : Filesystem::Stat(Resolved);
        ProcessPackage(Path, Stat.bValid ? Stat.LastModifyTime : 0, Stat.bValid ? Stat.Size : 0);
    }

    void FAssetRegistry::ProcessPackage(FStringView Path, int64 MTimeNs, uint64 FileSize)
    {
        LUMINA_MEMORY_SCOPE("Asset Registry");
        if (MatchesCachedStamp(Path, MTimeNs, FileSize))
        {
            DiscoveryUnchanged.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // The hash covers raw compressed bytes, so a source or compression change invalidates it.
        TVector<uint8> RawBytes;
        const uint64 Hash = ContentHashOf(Path, &RawBytes);
        if (Hash != 0 && RefreshStampIfContentUnchanged(Path, Hash, MTimeNs, FileSize))
        {
            DiscoveryRehashed.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        DiscoveryExtracted.fetch_add(1, std::memory_order_relaxed);

        if (RawBytes.IsEmpty())
        {
            LOG_ERROR("AssetRegistry: failed to read {}", Path);
            return;
        }

        // Decompressed into Bytes, which holds the header and import and export tables.
        TVector<uint8> Bytes;
        FPackageHeader Header;
        if (!CPackage::ReadPackageContainer(Path, Bytes, Header))
        {
            LOG_ERROR("AssetRegistry: refusing to register {}", Path);
            return;
        }

        FName PackageFileName = VFS::FileName(Path, true);

        FPackageContainerReader Reader(Bytes);
        Reader.SetFileVersion(Header.Version);

        // the export and import tables name things through slots, so the table comes first
        FPackageNameTable Names;
        if (!CPackage::ReadNameTable(Reader, Header, Names))
        {
            LOG_ERROR("AssetRegistry: {} has an unreadable name table", Path);
            return;
        }
        Reader.SetNameTable(&Names);

        Reader.Seek(Header.ExportTableOffset);

        TVector<FObjectExport> Exports;
        Reader << Exports;

        FObjectExport* Export = Algo::FindIf(Exports, [&](const FObjectExport& E)
        {
            return E.ObjectName == PackageFileName;
        });

        if (Export == Exports.end())
        {
            LOG_ERROR("AssetRegistry: {} contains no export matching its file name; refusing to register", Path);
            return;
        }

        // Hard for a direct CObject reference and Soft for an FSoftObjectPath, as the saver recorded.
        TVector<FAssetDependency> Dependencies;
        TVector<FAssetBundleEntry> Bundles;
        if (Header.ImportTableOffset >= 0
            && Header.ImportTableOffset <= Bytes.Num())
        {
            Reader.Seek(Header.ImportTableOffset);
            TVector<FObjectImport> Imports;
            Reader << Imports;
            Dependencies.Reserve(Imports.Num());
            for (const FObjectImport& Import : Imports)
            {
                FAssetDependency Dep;
                Dep.TargetGUID = Import.ObjectGUID;
                Dep.Type       = Import.Type;
                Dependencies.Emplace(Dep);
                for (const FName& Bundle : Import.Bundles)
                {
                    Bundles.Emplace(Bundle, Import.ObjectGUID);
                }
            }
        }

        auto AssetData = MakeShared<FAssetData>();
        AssetData->AssetClass     = Export->ClassName;
        AssetData->AssetGUID      = Export->ObjectGUID;
        AssetData->AssetName      = Export->ObjectName;
        AssetData->Path = Path;
        AssetData->ContentHash    = Hash;
        AssetData->SourceMTimeNs  = MTimeNs;
        AssetData->FileSize       = FileSize;
        AssetData->Dependencies   = Move(Dependencies);
        AssetData->Bundles        = Move(Bundles);
        AssetData->OwningPlugin   = ExtractOwningPlugin(Path);

        FWriteScopeLock Lock(AssetsMutex);
        InsertAssetLocked(Move(AssetData));
    }

    void FAssetRegistry::ClearAssets()
    {
        // Listeners read the registry straight back, and AssetsMutex is not recursive.
        {
            FWriteScopeLock Lock(AssetsMutex);
            InvalidatePathIndex();
            Assets.Reset();

            {
                FWriteScopeLock TextLock(TextAssetsMutex);
                TextAssets.Reset();
                bTextPathIndexValid = false;
            }

            {
                FWriteScopeLock RLock(ReverseMapMutex);
                ReverseDepMap.Reset();
                bReverseMapDirty = false;
            }
        }

        DispatchRegistryChanged();
    }

    void FAssetRegistry::WriteToArchive(FArchive& Ar) const
    {
        uint32 Tag = kAssetRegistryCacheTag;
        Ar << Tag;

        FReadScopeLock Lock(AssetsMutex);
        uint32 Count = (uint32)Assets.Num();
        Ar << Count;

        for (const FAssetDataPtr& Listed : Assets)
        {
            // A writing archive only reads, though it takes the fields by mutable reference.
            FAssetData* Data = const_cast<FAssetData*>(Listed.Get());
            Ar << Data->AssetGUID;
            Ar << Data->Path;
            Ar << Data->AssetName;
            Ar << Data->AssetClass;
            Ar << Data->ContentHash;
            Ar << Data->SourceMTimeNs;
            uint32 Flags = (uint32)Data->Flags;
            Ar << Flags;

            uint32 DepCount = (uint32)Data->Dependencies.Num();
            Ar << DepCount;
            for (const FAssetDependency& Dep : Data->Dependencies)
            {
                Ar << const_cast<FGuid&>(Dep.TargetGUID);
                uint8 T = (uint8)Dep.Type;
                Ar << T;
            }

            Ar << Data->OwnerChunk;
            Ar << Data->OwningPlugin;

            uint32 BundleCount = (uint32)Data->Bundles.Num();
            Ar << BundleCount;
            for (const FAssetBundleEntry& Entry : Data->Bundles)
            {
                Ar << const_cast<FName&>(Entry.Bundle);
                Ar << const_cast<FGuid&>(Entry.Target);
            }
        }

        // Text-asset identity table (v2+).
        FReadScopeLock TextLock(TextAssetsMutex);
        uint32 TextCount = (uint32)TextAssets.Num();
        Ar << TextCount;
        for (const FTextAssetDataPtr& Data : TextAssets)
        {
            FTextAssetData& Entry = const_cast<FTextAssetData&>(*Data);
            Ar << Entry.Guid;
            Ar << Entry.Path;
            Ar << Entry.Name;
            uint8 Kind = (uint8)Entry.Kind;
            Ar << Kind;
            Ar << Entry.OwningPlugin;
            Ar << Entry.SourceMTimeNs;
        }
    }

    bool FAssetRegistry::LoadFromArchive(FArchive& Ar)
    {
        LUMINA_MEMORY_SCOPE("Asset Registry");
        uint32 Tag = 0;
        Ar << Tag;
        if (Tag != kAssetRegistryCacheTag)
        {
            return false;
        }

        uint32 Count = 0;
        Ar << Count;
        if (!Ar.CanHoldCount(Count))
        {
            return false;
        }

        FWriteScopeLock Lock(AssetsMutex);
        InvalidatePathIndex();
        Assets.Reset();
        Assets.Reserve(Count);

        for (uint32 i = 0; i < Count && !Ar.HasError(); ++i)
        {
            auto Data = MakeShared<FAssetData>();
            Ar << Data->AssetGUID;
            Ar << Data->Path;
            Ar << Data->AssetName;
            Ar << Data->AssetClass;
            Ar << Data->ContentHash;
            Ar << Data->SourceMTimeNs;
            uint32 Flags = 0;
            Ar << Flags;
            Data->Flags = (EAssetFlags)Flags;

            uint32 DepCount = 0;
            Ar << DepCount;
            if (!Ar.CanHoldCount(DepCount))
            {
                Ar.SetHasError(true);
                break;
            }
            Data->Dependencies.SetNum(DepCount);
            for (uint32 d = 0; d < DepCount; ++d)
            {
                Ar << Data->Dependencies[d].TargetGUID;
                uint8 T = 0;
                Ar << T;
                Data->Dependencies[d].Type = (EDependencyType)T;
            }

            Ar << Data->OwnerChunk;
            Ar << Data->OwningPlugin;

            uint32 BundleCount = 0;
            Ar << BundleCount;
            if (!Ar.CanHoldCount(BundleCount))
            {
                Ar.SetHasError(true);
                break;
            }
            Data->Bundles.SetNum(BundleCount);
            for (FAssetBundleEntry& Entry : Data->Bundles)
            {
                Ar << Entry.Bundle;
                Ar << Entry.Target;
            }

            Assets.TryAdd(Move(Data));
        }

        // Text-asset identity table (v2+).
        {
            FWriteScopeLock TextLock(TextAssetsMutex);
            TextAssets.Reset();
            bTextPathIndexValid = false;

            uint32 TextCount = 0;
            Ar << TextCount;
            if (!Ar.CanHoldCount(TextCount))
            {
                Ar.SetHasError(true);
                TextCount = 0;
            }
            TextAssets.Reserve(TextCount);
            for (uint32 i = 0; i < TextCount && !Ar.HasError(); ++i)
            {
                auto Data = MakeShared<FTextAssetData>();
                Ar << Data->Guid;
                Ar << Data->Path;
                Ar << Data->Name;
                uint8 Kind = 0;
                Ar << Kind;
                Data->Kind = (ETextAssetKind)Kind;
                Ar << Data->OwningPlugin;
                Ar << Data->SourceMTimeNs;

                AddTextAssetLocked(Move(Data));
            }
        }

        {
            FWriteScopeLock RLock(ReverseMapMutex);
            bReverseMapDirty = true;
        }

        return !Ar.HasError();
    }

    // FGuid and FFixedString have no leaf overload, so they round-trip through an FString.
    namespace
    {
        // A JSON cache cost 1.8 s to parse and 2 s to write at 109k entries, so the cache is a flat binary stream.
        constexpr uint32 kAssetRegistryCacheMagic   = 0x4352414C;
        constexpr uint32 kAssetRegistryCacheVersion = 2;

        // A version 1 cache predates bundles, and every package it describes was saved without any.
        constexpr uint32 kAssetRegistryCacheBundlesVersion = 2;

        // A damaged count is refused before it sizes an allocation.
        constexpr uint32 kMaxCachedDependencies = 1u << 16;

        void SerializeName(FArchive& Ar, FName& Name)
        {
            FString Text;
            if (Ar.IsWriting() && !Name.IsNone())
            {
                Text = *Name;
            }
            Ar << Text;
            if (Ar.IsReading())
            {
                Name = Text.IsEmpty() ? FName() : FName(Text);
            }
        }

        void SerializeAssetEntry(FArchive& Ar, FAssetData& Data, uint32 Version)
        {
            Ar << Data.AssetGUID;
            Ar << Data.Path;
            SerializeName(Ar, Data.AssetName);
            SerializeName(Ar, Data.AssetClass);
            Ar << Data.ContentHash;
            Ar << Data.SourceMTimeNs;
            Ar << Data.FileSize;

            uint32 Flags = (uint32)Data.Flags;
            Ar << Flags;
            Data.Flags = (EAssetFlags)Flags;

            SerializeName(Ar, Data.OwnerChunk);
            SerializeName(Ar, Data.OwningPlugin);

            uint32 DependencyCount = (uint32)Data.Dependencies.Num();
            Ar << DependencyCount;
            if (Ar.IsReading())
            {
                if (DependencyCount > kMaxCachedDependencies)
                {
                    Ar.SetHasError(true);
                    return;
                }
                Data.Dependencies.SetNum(DependencyCount);
            }
            for (FAssetDependency& Dependency : Data.Dependencies)
            {
                Ar << Dependency.TargetGUID;
                uint8 Type = (uint8)Dependency.Type;
                Ar << Type;
                Dependency.Type = (EDependencyType)Type;
            }

            if (Version < kAssetRegistryCacheBundlesVersion)
            {
                return;
            }
            uint32 BundleCount = (uint32)Data.Bundles.Num();
            Ar << BundleCount;
            if (Ar.IsReading())
            {
                if (BundleCount > kMaxCachedDependencies)
                {
                    Ar.SetHasError(true);
                    return;
                }
                Data.Bundles.SetNum(BundleCount);
            }
            for (FAssetBundleEntry& Entry : Data.Bundles)
            {
                SerializeName(Ar, Entry.Bundle);
                Ar << Entry.Target;
            }
        }

        void SerializeTextEntry(FArchive& Ar, FTextAssetData& Data)
        {
            Ar << Data.Guid;
            Ar << Data.Path;
            SerializeName(Ar, Data.Name);

            uint8 Kind = (uint8)Data.Kind;
            Ar << Kind;
            Data.Kind = (ETextAssetKind)Kind;

            SerializeName(Ar, Data.OwningPlugin);
            Ar << Data.SourceMTimeNs;
        }
    }

    void FAssetRegistry::SaveCache() const
    {
        const FString CachePath = AssetDbPath();
        if (CachePath.IsEmpty()) return;

        Filesystem::MakeParentDirectoryTree(CachePath);

        TVector<uint8> Bytes;
        FMemoryWriter Writer(Bytes);

        uint32 Magic   = kAssetRegistryCacheMagic;
        uint32 Version = kAssetRegistryCacheVersion;
        Writer << Magic;
        Writer << Version;

        {
            FReadScopeLock Lock(AssetsMutex);
            uint32 Count = (uint32)Assets.Num();
            Writer << Count;
            for (const FAssetDataPtr& Data : Assets)
            {
                SerializeAssetEntry(Writer, const_cast<FAssetData&>(*Data), Version);
            }
        }

        {
            FReadScopeLock TextLock(TextAssetsMutex);
            uint32 Count = (uint32)TextAssets.Num();
            Writer << Count;
            for (const FTextAssetDataPtr& Data : TextAssets)
            {
                SerializeTextEntry(Writer, const_cast<FTextAssetData&>(*Data));
            }
        }

        if (!FileHelper::SaveArrayToFile(Bytes, CachePath))
        {
            LOG_WARN("AssetRegistry: failed to write cache to {}", CachePath);
        }
    }

    bool FAssetRegistry::LoadCache()
    {
        LUMINA_MEMORY_SCOPE("Asset Registry");
        const FString CachePath = AssetDbPath();
        if (CachePath.IsEmpty()) return false;

        // A first launch has no cache, so exit quietly and let the full rescan handle it.
        if (!Filesystem::Exists(CachePath))
        {
            return false;
        }

        TVector<uint8> Bytes;
        if (!FileHelper::LoadFileToArray(Bytes, CachePath) || Bytes.IsEmpty())
        {
            return false;
        }

        FMemoryReader Reader(Bytes);
        uint32 Magic   = 0;
        uint32 Version = 0;
        Reader << Magic;
        Reader << Version;
        if (Reader.HasError() || Magic != kAssetRegistryCacheMagic || Version > kAssetRegistryCacheVersion || Version < 1)
        {
            LOG_INFO("AssetRegistry: cache at {} is from another build; rebuilding from scratch", CachePath);
            return false;
        }

        // Read whole before anything is replaced, so a damaged file leaves the registry as it was.
        uint32 AssetCount = 0;
        Reader << AssetCount;
        TVector<FAssetDataPtr> LoadedAssets;
        LoadedAssets.Reserve(Reader.HasError() ? 0 : Math::Min<uint32>(AssetCount, (uint32)(Bytes.Num() / 32)));
        for (uint32 i = 0; i < AssetCount && !Reader.HasError(); ++i)
        {
            auto Data = MakeShared<FAssetData>();
            SerializeAssetEntry(Reader, *Data, Version);
            LoadedAssets.Add(Move(Data));
        }

        uint32 TextCount = 0;
        Reader << TextCount;
        TVector<FTextAssetDataPtr> LoadedText;
        for (uint32 i = 0; i < TextCount && !Reader.HasError(); ++i)
        {
            auto Data = MakeShared<FTextAssetData>();
            SerializeTextEntry(Reader, *Data);
            LoadedText.Add(Move(Data));
        }

        if (Reader.HasError())
        {
            LOG_WARN("AssetRegistry: cache at {} is damaged; rebuilding from scratch", CachePath);
            return false;
        }

        {
            FWriteScopeLock Lock(AssetsMutex);
            InvalidatePathIndex();
            Assets.Reset();
            Assets.Reserve(LoadedAssets.Num());
            for (FAssetDataPtr& Data : LoadedAssets)
            {
                Assets.TryAdd(Move(Data));
            }
        }

        {
            FWriteScopeLock TextLock(TextAssetsMutex);
            TextAssets.Reset();
            bTextPathIndexValid = false;
            TextAssets.Reserve(LoadedText.Num());
            for (FTextAssetDataPtr& Data : LoadedText)
            {
                AddTextAssetLocked(Move(Data));
            }
        }

        {
            FWriteScopeLock RLock(ReverseMapMutex);
            bReverseMapDirty = true;
        }

        LOG_INFO("AssetRegistry: loaded {} entries from cache {}", Assets.Num(), CachePath);
        return true;
    }
}
