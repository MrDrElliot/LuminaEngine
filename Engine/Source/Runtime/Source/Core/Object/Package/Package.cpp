#include "Platform/Time/PlatformTime.h"
#include "RuntimePCH.h"
#include "Package.h"
#include "Core/Profiler/AssetLoadTracker.h"
#include <utility>
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Object/Archive/ObjectReferenceReplacerArchive.h"
#include "Core/Profiler/Profile.h"
#include "Memory/MemoryTracking.h"
#include "Core/Serialization/Package/PackageLoader.h"
#include "Core/Serialization/Package/PackageSaver.h"
#include "FileSystem/FileSystem.h"
#include "Paths/Paths.h"
#include "TaskSystem/TaskSystem.h"
#include "Thumbnail/PackageThumbnail.h"
#include "miniz.h"
#include "Log/Log.h"
#include "Core/Templates/IntegerCompare.h"


namespace Lumina
{
    IMPLEMENT_INTRINSIC_CLASS(CPackage, CObject, RUNTIME_API)

    FPackageDestroyedDelegate CPackage::OnPackageDestroyed;

    namespace
    {
        // Legacy single-stream container (still read for packages saved before chunking landed).
        struct FCompressedPackageHeader
        {
            uint64 UncompressedSize;
            uint64 CompressedSize;
        };

        // Chunked container.
        constexpr uint32 kPackageChunkMagic   = 0x32435A4C; // 'LZC2'
        constexpr uint32 kPackageChunkVersion = 1;
        constexpr uint32 kPackageChunkSize    = 4u * 1024 * 1024; // 4 MiB uncompressed per chunk

        // Putting the location at the END keeps the header wire-compatible with no two-pass save.
        constexpr uint32 kBulkTrailerMagic   = 0x4B4C424C; // 'LBLK'
        constexpr uint32 kBulkTrailerVersion = 1;

        struct FPackageBulkTrailer
        {
            uint32 Magic;
            uint32 Version;
            uint64 BulkOffset;   // file offset of the raw bulk region == size of the compressed container
            uint64 BulkSize;
        };
        static_assert(std::is_trivially_copyable_v<FPackageBulkTrailer>);

        // Above the limit the tail is read first, so a huge bulk region is never pulled needlessly.
        constexpr uint64 kWholeFileReadLimit = 1u * 1024 * 1024;

        // TailBytes must be the LAST bytes of the file, and the offsets are validated against its length.
        bool ParseBulkTrailer(const uint8* TailBytes, size_t TailSize, uint64 FileSize, CPackage::FBulkRegion& Out)
        {
            Out = CPackage::FBulkRegion{};

            if (TailBytes == nullptr || TailSize < sizeof(FPackageBulkTrailer))
            {
                return false;
            }

            FPackageBulkTrailer Trailer;
            std::memcpy(&Trailer, TailBytes + TailSize - sizeof(FPackageBulkTrailer), sizeof(Trailer));

            if (Trailer.Magic != kBulkTrailerMagic || Trailer.Version != kBulkTrailerVersion)
            {
                return false;
            }

            // The region has to sit entirely between the container and the trailer itself.
            const uint64 TrailerStart = FileSize - sizeof(FPackageBulkTrailer);
            if (Trailer.BulkOffset > TrailerStart || Trailer.BulkSize > TrailerStart - Trailer.BulkOffset)
            {
                LOG_ERROR("ParseBulkTrailer: bulk region (offset={}, size={}) does not fit a {}-byte file",
                    Trailer.BulkOffset, Trailer.BulkSize, FileSize);
                return false;
            }

            Out.FileOffset = (int64)Trailer.BulkOffset;
            Out.Size       = (int64)Trailer.BulkSize;
            return true;
        }

        bool DeflateChunk(const uint8* Src, size_t Len, TVector<uint8>& Out)
        {
            mz_ulong Bound = mz_compressBound((mz_ulong)Len);
            Out.resize((size_t)Bound);
            mz_ulong OutLen = Bound;
            const int Ret = mz_compress2(Out.data(), &OutLen, Src, (mz_ulong)Len, MZ_DEFAULT_LEVEL);
            if (Ret != MZ_OK)
            {
                Out.clear();
                return false;
            }
            Out.resize((size_t)OutLen);
            return true;
        }

        // Deflate In into the chunked container; chunks compress in parallel (one task each).
        bool CompressPackageBinary(const TVector<uint8>& In, TVector<uint8>& Out)
        {
            LUMINA_PROFILE_SCOPE();

            const uint64 Total     = In.size();
            const uint32 NumChunks = (Total == 0) ? 1u : (uint32)((Total + kPackageChunkSize - 1) / kPackageChunkSize);

            TVector<TVector<uint8>> ChunkBytes(NumChunks);
            TVector<uint8>          ChunkOk(NumChunks, 0);

            auto CompressOne = [&](uint32 i)
            {
                const size_t Start = (size_t)i * kPackageChunkSize;
                const size_t Len   = (size_t)((Total - Start) < kPackageChunkSize ? (Total - Start) : kPackageChunkSize);
                ChunkOk[i] = DeflateChunk(In.data() + Start, Len, ChunkBytes[i]) ? 1 : 0;
            };

            if (NumChunks <= 1)
            {
                CompressOne(0);
            }
            else
            {
                Task::ParallelFor(NumChunks, CompressOne, 1);
            }

            for (uint32 i = 0; i < NumChunks; ++i)
            {
                if (ChunkOk[i] == 0)
                {
                    LOG_ERROR("CompressPackageBinary: chunk {} failed to compress", i);
                    Out.clear();
                    return false;
                }
            }

            size_t TotalCompressed = 0;
            for (const TVector<uint8>& Chunk : ChunkBytes)
            {
                TotalCompressed += Chunk.size();
            }

            const size_t HeaderBytes = sizeof(uint32) * 2 + sizeof(uint64) + sizeof(uint32) * 2
                                     + (size_t)NumChunks * sizeof(uint32);
            Out.resize(HeaderBytes + TotalCompressed);

            uint8* P = Out.data();
            auto WriteU32 = [&P](uint32 V) { std::memcpy(P, &V, sizeof(V)); P += sizeof(V); };
            auto WriteU64 = [&P](uint64 V) { std::memcpy(P, &V, sizeof(V)); P += sizeof(V); };

            WriteU32(kPackageChunkMagic);
            WriteU32(kPackageChunkVersion);
            WriteU64(Total);
            WriteU32(kPackageChunkSize);
            WriteU32(NumChunks);
            for (const TVector<uint8>& Chunk : ChunkBytes)
            {
                WriteU32((uint32)Chunk.size());
            }
            for (const TVector<uint8>& Chunk : ChunkBytes)
            {
                std::memcpy(P, Chunk.data(), Chunk.size());
                P += Chunk.size();
            }

            return true;
        }

        bool DecompressChunkedPackage(const TVector<uint8>& Raw, TVector<uint8>& Out)
        {
            const uint8*  P     = Raw.data();
            const size_t  Size  = Raw.size();
            size_t        Off   = 0;

            const size_t FixedHeader = sizeof(uint32) * 2 + sizeof(uint64) + sizeof(uint32) * 2;
            if (Size < FixedHeader)
            {
                LOG_ERROR("DecompressChunkedPackage: truncated header");
                return false;
            }

            auto ReadU32 = [&]() { uint32 V; std::memcpy(&V, P + Off, sizeof(V)); Off += sizeof(V); return V; };
            auto ReadU64 = [&]() { uint64 V; std::memcpy(&V, P + Off, sizeof(V)); Off += sizeof(V); return V; };

            const uint32 Magic     = ReadU32();
            const uint32 Version   = ReadU32();
            const uint64 Total     = ReadU64();
            const uint32 ChunkSize = ReadU32();
            const uint32 NumChunks = ReadU32();

            if (Magic != kPackageChunkMagic || Version != kPackageChunkVersion || ChunkSize == 0)
            {
                LOG_ERROR("DecompressChunkedPackage: bad header (magic/version/chunkSize)");
                return false;
            }

            if (Size < FixedHeader + (size_t)NumChunks * sizeof(uint32))
            {
                LOG_ERROR("DecompressChunkedPackage: truncated size table");
                return false;
            }

            TVector<uint32> Sizes(NumChunks);
            TVector<size_t> Offsets(NumChunks);
            size_t DataOff = FixedHeader + (size_t)NumChunks * sizeof(uint32);
            for (uint32 i = 0; i < NumChunks; ++i)
            {
                Sizes[i]   = ReadU32();
                Offsets[i] = DataOff;
                DataOff   += Sizes[i];
            }
            if (DataOff > Size)
            {
                LOG_ERROR("DecompressChunkedPackage: chunk data overruns file");
                return false;
            }

            Out.resize((size_t)Total);

            // A single-chunk package stays inline to avoid a task hop on the registry-discovery path.
            TVector<uint8> ChunkOk(NumChunks, 1);

            auto InflateOne = [&](uint32 i)
            {
                const size_t OutStart = (size_t)i * ChunkSize;
                const size_t Expected = (size_t)(((uint64)OutStart + ChunkSize <= Total) ? ChunkSize : (Total - OutStart));
                mz_ulong OutLen = (mz_ulong)Expected;
                const int Ret = mz_uncompress(Out.data() + OutStart, &OutLen, P + Offsets[i], (mz_ulong)Sizes[i]);
                if (Ret != MZ_OK || OutLen != Expected)
                {
                    LOG_ERROR("DecompressChunkedPackage: chunk {} inflate failed (ret={}, got={}, expected={})",
                        i, Ret, (uint64)OutLen, (uint64)Expected);
                    ChunkOk[i] = 0;
                }
            };

            if (NumChunks <= 1)
            {
                InflateOne(0);
            }
            else
            {
                Task::ParallelFor(NumChunks, InflateOne, 1);
            }

            for (uint32 i = 0; i < NumChunks; ++i)
            {
                if (ChunkOk[i] == 0)
                {
                    Out.clear();
                    return false;
                }
            }

            return true;
        }

        bool DecompressPackageBinary(const TVector<uint8>& Raw, TVector<uint8>& Out)
        {
            // A legacy stream starts with an uncompressed size that cannot collide with the magic.
            if (Raw.size() >= sizeof(uint32))
            {
                uint32 Magic;
                std::memcpy(&Magic, Raw.data(), sizeof(Magic));
                if (Magic == kPackageChunkMagic)
                {
                    return DecompressChunkedPackage(Raw, Out);
                }
            }

            if (Raw.size() < sizeof(FCompressedPackageHeader))
            {
                LOG_ERROR("DecompressPackageBinary: file too small ({} bytes)", Raw.size());
                return false;
            }

            FCompressedPackageHeader CHeader;
            std::memcpy(&CHeader, Raw.data(), sizeof(CHeader));

            if (sizeof(FCompressedPackageHeader) + CHeader.CompressedSize != Raw.size())
            {
                LOG_ERROR("DecompressPackageBinary: size mismatch (header={}, file={})",
                    sizeof(FCompressedPackageHeader) + CHeader.CompressedSize, Raw.size());
                return false;
            }

            Out.resize((size_t)CHeader.UncompressedSize);
            mz_ulong OutLen = (mz_ulong)CHeader.UncompressedSize;
            int Ret = mz_uncompress(Out.data(), &OutLen,
                Raw.data() + sizeof(FCompressedPackageHeader), (mz_ulong)CHeader.CompressedSize);

            if (Ret != MZ_OK || OutLen != CHeader.UncompressedSize)
            {
                LOG_ERROR("DecompressPackageBinary: decompress failed (ret={}, got={}, expected={})",
                    Ret, (uint64)OutLen, CHeader.UncompressedSize);
                Out.clear();
                return false;
            }

            return true;
        }
    }

    namespace
    {
        // Loads run in parallel now, so an export is claimed before it is read. The owner deserializes,
        // anyone else waits for it, and a load that re-enters its own object takes the partial one, which
        // is how a reference cycle resolves.
        FMutex                            GClaimMutex;
        FConditionVariable                GClaimSignal;
        THashMap<const CObject*, uint64>  GClaimOwners;

        enum class EClaimResult : uint8
        {
            Owned,       // this thread must load it
            AlreadyDone, // someone finished it while we waited
            Reentrant,   // this thread is already loading it, so the partial object is the answer
        };

        EClaimResult ClaimObjectForLoad(CObject* Object)
        {
            const uint64 ThisThread = (uint64)Threading::GetThreadID();

            FUniqueLock Lock(GClaimMutex);
            for (;;)
            {
                if (!Object->HasAnyFlag(OF_NeedsLoad))
                {
                    return EClaimResult::AlreadyDone;
                }

                const auto It = GClaimOwners.find(Object);
                if (It == GClaimOwners.end())
                {
                    GClaimOwners.emplace(Object, ThisThread);
                    Object->SetFlag(OF_Loading);
                    return EClaimResult::Owned;
                }

                if (It->second == ThisThread)
                {
                    return EClaimResult::Reentrant;
                }

                GClaimSignal.Wait(Lock);
            }
        }

        void ReleaseObjectLoad(CObject* Object)
        {
            {
                FScopeLock Lock(GClaimMutex);
                GClaimOwners.erase(Object);
            }
            GClaimSignal.NotifyAll();
        }

        thread_local bool          GtDeferPostLoad = false;
        
        const THashSet<CPackage*>* GGraphClosure = nullptr;

        bool ShouldDeferPostLoad(const CObject* Object)
        {
            return GtDeferPostLoad
                && Object != nullptr
                && GGraphClosure != nullptr
                && GGraphClosure->find(Object->GetPackage()) != GGraphClosure->end();
        }
    }

    bool CPackage::ReadPackageFile(FStringView Path, TVector<uint8>& OutBinary, FBulkRegion* OutBulkRegion)
    {
        if (OutBulkRegion)
        {
            *OutBulkRegion = FBulkRegion{};
        }

        const uint64 FileSize = (uint64)VFS::Size(Path);

        // Splitting a small package into two reads would double the IO count on a world load for nothing.
        if (FileSize <= kWholeFileReadLimit)
        {
            TVector<uint8> RawBinary;
            if (!VFS::ReadFile(RawBinary, Path))
            {
                return false;
            }

            FBulkRegion Region;
            if (ParseBulkTrailer(RawBinary.data(), RawBinary.size(), (uint64)RawBinary.size(), Region))
            {
                // The bulk bytes and trailer are not part of the deflate stream, so trim to the container.
                RawBinary.resize((size_t)Region.FileOffset);
                if (OutBulkRegion)
                {
                    *OutBulkRegion = Region;
                }
            }

            return DecompressPackageBinary(RawBinary, OutBinary);
        }

        // A large file checks for a bulk region before reading anything big.
        uint64 ContainerSize = FileSize;

        TVector<uint8> Tail;
        if (VFS::ReadFileRange(Tail, Path, FileSize - sizeof(FPackageBulkTrailer), sizeof(FPackageBulkTrailer)))
        {
            FBulkRegion Region;
            if (ParseBulkTrailer(Tail.data(), Tail.size(), FileSize, Region))
            {
                ContainerSize = (uint64)Region.FileOffset;
                if (OutBulkRegion)
                {
                    *OutBulkRegion = Region;
                }
            }
        }

        TVector<uint8> RawBinary;
        if (!VFS::ReadFileRange(RawBinary, Path, 0, ContainerSize))
        {
            return false;
        }

        return DecompressPackageBinary(RawBinary, OutBinary);
    }

    bool CPackage::ReadBulkData(const FBulkDataRef& Ref, TVector<uint8>& OutBytes, uint32 ExpectedGeneration) const
    {
        OutBytes.clear();

        if (!Ref.IsValid())
        {
            return false;
        }

        // A save replaces the file and republishes the region as two steps, so a read starting between them
        // would apply the old offsets to the new bytes and come back with the right size of the wrong payload.
        for (int32 Attempt = 0; Attempt < 3; ++Attempt)
        {
            // Tracked explicitly rather than derived from the name, which mid-rename points at a missing file.
            FBulkRegion  Region;
            FFixedString Path;
            uint32       Generation = 0;
            {
                FScopeLock Lock(BulkMutex);
                Region     = BulkRegion;
                Path       = BulkSourcePath.empty() ? GetPackagePath() : BulkSourcePath;
                Generation = BulkGeneration;
            }

            // The caller captured this ref against an earlier layout, so it no longer names anything.
            if (ExpectedGeneration != 0 && Generation != ExpectedGeneration)
            {
                LOG_ERROR("ReadBulkData: {} was saved since this ref was taken, so the payload it named is gone", GetName());
                return false;
            }

            if (!Region.IsValid() || Ref.Offset < 0 || Ref.Size > Region.Size - Ref.Offset)
            {
                LOG_ERROR("ReadBulkData: ref (offset={}, size={}) is outside package {}'s bulk region (size={})",
                    Ref.Offset, Ref.Size, GetName(), Region.Size);
                return false;
            }

            if (!VFS::ReadFileRange(OutBytes, Path, (uint64)(Region.FileOffset + Ref.Offset), (uint64)Ref.Size))
            {
                LOG_ERROR("ReadBulkData: ranged read failed for {}", Path);
                OutBytes.clear();
                return false;
            }

            // A short read means the file changed under us (re-saved, truncated); the payload is unusable.
            if ((int64)OutBytes.size() != Ref.Size)
            {
                LOG_ERROR("ReadBulkData: short read for {} (wanted {}, got {})", Path, Ref.Size, OutBytes.size());
                OutBytes.clear();
                return false;
            }

            // Same size out of a different file is what nothing above can see, so the generation catches it.
            {
                FScopeLock Lock(BulkMutex);
                if (BulkGeneration == Generation)
                {
                    return true;
                }
            }

            OutBytes.clear();
        }

        LOG_ERROR("ReadBulkData: {} kept being saved underneath the read", GetName());
        return false;
    }

    FObjectExport::FObjectExport(CObject* InObject)
    {
        ObjectGUID      = InObject->GetGUID();
        ObjectName      = InObject->GetName();
        ClassName       = InObject->GetClass()->GetName();
        Offset          = 0;
        Size            = 0;
        Object          = InObject;
    }

    FObjectImport::FObjectImport(CObject* InObject)
    {
        ObjectGUID      = InObject->GetGUID();
        Object          = InObject;
    }
    
    void CPackage::OnDestroy()
    {
        
    }

    bool CPackage::Rename(const FName& NewName, CPackage* NewPackage)
    {
        // FName::ToString returns by value, so bind to locals or the string views dangle.
        const FString NewNameStr = NewName.ToString();
        const FString OldNameStr = GetName().ToString();
        FStringView FileName = VFS::FileName(NewNameStr, true);
        FStringView OldFileName = VFS::FileName(OldNameStr, true);
        bool bFileNameDirty = FileName != OldFileName;

        if (bFileNameDirty)
        {
            // Use live object set, not ExportTable (cleared each save; stale weak ptrs otherwise).
            const FName OldFileNameAsFName(OldFileName);
            const FName NewFileNameAsFName(FileName);

            TVector<CObject*> PackageObjects;
            PackageObjects.reserve(8);
            GetObjectsWithPackage(this, PackageObjects);

            for (CObject* Object : PackageObjects)
            {
                if (Object && Object->GetName() == OldFileNameAsFName)
                {
                    Object->Rename(NewFileNameAsFName, nullptr);
                }
            }

            for (FObjectExport& Export : ExportTable)
            {
                if (Export.ObjectName == OldFileNameAsFName)
                {
                    Export.ObjectName = NewFileNameAsFName;
                }
            }
        }

        return Super::Rename(NewName, NewPackage);
    }

    CPackage* CPackage::CreatePackage(FStringView Path)
    {
        FFixedString ObjectName = SanitizeObjectName(Path);
        if (FindObject<CPackage>(ObjectName) != nullptr)
        {
            LOG_ERROR("CreatePackage: package {} already exists", Path);
            return nullptr;
        }

        CPackage* Package = NewObject<CPackage>(nullptr, ObjectName);
        Package->AddToRoot();

        LOG_INFO("Created Package: \"{}\"", Path);

        Package->MarkDirty();

        return Package;
    }

    CPackage* CPackage::GetTransientPackage()
    {
        static CPackage* TransientPackage = nullptr;
        if (TransientPackage == nullptr)
        {
            TransientPackage = NewObject<CPackage>(nullptr, "EngineTransient");
            TransientPackage->AddToRoot();
            TransientPackage->SetFlag(OF_Transient);
            TransientPackage->LoadState.store(ELoadState::Loaded, std::memory_order_release);
        }
        return TransientPackage;
    }

    bool CPackage::IsTransientPackage() const
    {
        return HasAnyFlag(OF_Transient);
    }
    
    bool CPackage::DestroyPackage(FStringView Path)
    {
        // If loaded, route through the live-reference replacement path.
        if (CPackage* Package = FindPackageByPath(Path))
        {
            return DestroyPackage(Package);
        }
        
        TVector<uint8> PackageBlob;
        if (!ReadPackageFile(Path, PackageBlob))
        {
            LOG_ERROR("Failed to load package file at path {}", Path);
            return false;
        }

        FPackageHeader Header;
        FMemoryReader Reader(PackageBlob);
        Reader << Header;

        Reader.Seek(Header.ExportTableOffset);
        
        TVector<FObjectExport> Exports;
        Reader << Exports;

        FName PackageFileName = VFS::FileName(Path, true);

        TOptional<FObjectExport> Export;
        for (const FObjectExport& E : Exports)
        {
            if (E.ObjectName == PackageFileName)
            {
                Export = E;
                break;
            }
        }

        if (!Export.has_value())
        {
            LOG_ERROR("No primary asset found in package");
            return false;
        }
        
        OnPackageDestroyed.Broadcast(Path);
        FAssetRegistry::Get().AssetDeleted(Export->ObjectGUID);
        VFS::Remove(Path);

        return true;
    }
    
    bool CPackage::DestroyPackage(CPackage* PackageToDestroy)
    {
        if (PackageToDestroy == nullptr || PackageToDestroy->HasAnyFlag(OF_MarkedDestroy))
        {
            return false;
        }

        if (PackageToDestroy->IsTransientPackage())
        {
            LOG_ERROR("DestroyPackage: refusing to destroy the engine transient package");
            return false;
        }

        FFixedString PackagePath = PackageToDestroy->GetPackagePath();

        (void)PackageToDestroy->FullyLoad();

        FName PackageFileName = VFS::FileName(PackagePath, true);
        FGuid AssetGUID;
        for (const FObjectExport& Export : PackageToDestroy->ExportTable)
        {
            if (Export.ObjectName == PackageFileName)
            {
                AssetGUID = Export.ObjectGUID;
                break;
            }
        }

        // Synchronous so the asset browser updates immediately.
        OnPackageDestroyed.Broadcast(PackagePath);

        if (AssetGUID.IsValid())
        {
            FAssetRegistry::Get().AssetDeleted(AssetGUID);
        }

        if (VFS::Exists(PackagePath) && !VFS::Remove(PackagePath))
        {
            LOG_ERROR("DestroyPackage: failed to remove package file {}", PackagePath);
        }

        // A deleted package has nothing to save; clear dirty so it can't surface in save prompts.
        PackageToDestroy->ClearDirty();

        // A deleted asset is unreachable by identity even while its husk is torn down below.
        PackageToDestroy->SetFlag(OF_MarkedDestroy);

        // Synchronous teardown.
        {
            // A primary asset's destructor releases sibling exports, which would die mid-loop without the pin.
            TVector<TObjectPtr<CObject>> PinnedExports;
            {
                TVector<CObject*> ExportObjects;
                ExportObjects.reserve(20);
                GetObjectsWithPackage(PackageToDestroy, ExportObjects);

                PinnedExports.reserve(ExportObjects.size());
                for (CObject* ExportObject : ExportObjects)
                {
                    PinnedExports.emplace_back(ExportObject);
                }
            }

            // Null every live reference to the exported assets across the whole object graph.
            for (const TObjectPtr<CObject>& ExportObject : PinnedExports)
            {
                if (!ExportObject.IsValid() || ExportObject.Get() == PackageToDestroy)
                {
                    continue;
                }
                if (!ExportObject->IsAsset())
                {
                    continue;
                }

                FObjectReferenceReplacerArchive Ar(ExportObject.Get(), nullptr);
                for (TObjectIterator<CObject> Itr; Itr; ++Itr)
                {
                    if (CObject* Object = *Itr)
                    {
                        Object->Serialize(Ar);
                    }
                }
            }
        }

        // Rebuilt from what is still alive, since the sweep may have freed pointers in the old list.
        TVector<CObject*> SurvivingExports;
        SurvivingExports.reserve(20);
        GetObjectsWithPackage(PackageToDestroy, SurvivingExports);

        // A no-op for any still held by a non-reflected strong ref, such as an open editor.
        for (CObject* ExportObject : SurvivingExports)
        {
            if (ExportObject == nullptr || ExportObject == PackageToDestroy)
            {
                continue;
            }
            if (ExportObject->HasAnyFlag(OF_MarkedDestroy))
            {
                continue;
            }

            ExportObject->ConditionalBeginDestroy();
        }

        PackageToDestroy->ExportTable.clear();
        PackageToDestroy->ImportTable.clear();
        PackageToDestroy->RemoveFromRoot();
        PackageToDestroy->ConditionalBeginDestroy();

        return true;
    }

    CPackage* CPackage::FindPackageByPath(FStringView Path)
    {
        FFixedString ObjectName = SanitizeObjectName(Path);
        return FindObject<CPackage>(ObjectName);
    }

    bool CPackage::RenamePackage(FStringView OldPath, FStringView NewPath)
    {
        if (OldPath == NewPath)
        {
            return true;
        }

        if (VFS::Exists(NewPath))
        {
            LOG_ERROR("RenamePackage: destination already exists: {}", NewPath);
            return false;
        }

        if (!VFS::Exists(OldPath))
        {
            LOG_ERROR("RenamePackage: source does not exist: {}", OldPath);
            return false;
        }

        FFixedString OldObjectName = SanitizeObjectName(OldPath);
        FFixedString NewObjectName = SanitizeObjectName(NewPath);

        // In-place export-table patching is unsafe, since an FName length prefix shifts offsets.
        CPackage* Package = FindObject<CPackage>(OldObjectName);
        if (Package == nullptr)
        {
            Package = LoadPackage(OldPath);
            if (Package == nullptr)
            {
                LOG_ERROR("RenamePackage: failed to load {} for rename", OldPath);
                return false;
            }
        }

        FName SavedName = Package->GetName();

        if (!Package->Rename(NewObjectName, nullptr))
        {
            LOG_ERROR("RenamePackage: in-memory rename failed for {}", OldPath);
            return false;
        }

        // The file still sits at OldPath, which is what lets PreSave pull streamed payloads back off disk.
        if (!SavePackage(Package, NewPath))
        {
            LOG_ERROR("RenamePackage: atomic save to {} failed; rolling back in-memory rename", NewPath);
            Package->Rename(SavedName, nullptr);
            return false;
        }

        // Stale duplicate on remove failure is preferable to data loss.
        if (VFS::Exists(OldPath) && !VFS::Remove(OldPath))
        {
            LOG_ERROR("RenamePackage: failed to remove old file {} (new file at {} is intact)", OldPath, NewPath);
        }
        return true;
    }

    void CPackage::OnPackageMovedExternally(FStringView OldPath, FStringView NewPath)
    {
        // A parent-directory rename already moved the file, so only in-memory identity updates.
        if (OldPath == NewPath)
        {
            return;
        }

        FFixedString OldObjectName = SanitizeObjectName(OldPath);
        if (CPackage* Package = FindObject<CPackage>(OldObjectName))
        {
            FFixedString NewObjectName = SanitizeObjectName(NewPath);
            Package->Rename(NewObjectName, nullptr);

            // The bytes really did move, so follow them or every later streamed read hits a missing path.
            if (!Package->BulkSourcePath.empty())
            {
                Package->SetBulkSource(Package->GetBulkRegion(), NewPath);
            }
        }
    }

    CPackage* CPackage::LoadPackage(FStringView Path)
    {
        LUMINA_PROFILE_SCOPE();
        LUMINA_MEMORY_SCOPE("Package Load");
        
        FFixedString ObjectName = SanitizeObjectName(Path);
        
        static FMutex FindOrCreateMutex;
        CPackage* Package = nullptr;
        {
            FScopeLock Lock(FindOrCreateMutex);
            Package = FindObject<CPackage>(ObjectName);
            
            if (Package == nullptr)
            {
                Package = NewObject<CPackage>(nullptr, ObjectName);
            }
        }
        
        const uint64 RequestStart = PlatformTime::Cycles();

        ELoadState Expected = ELoadState::Unloaded;
        bool bIsLoaderThread = Package->LoadState.compare_exchange_strong(
            Expected, 
            ELoadState::Loading,
            std::memory_order_acquire,
            std::memory_order_acquire);
        
        if (!bIsLoaderThread)
        {
            // Distinguished before the spin, since an already-loaded package never enters it.
            const bool bJoinedInFlight = Expected == ELoadState::Loading;

            if (bJoinedInFlight)
            {
                ELoadState State;
                while ((State = Package->LoadState.load(std::memory_order_acquire)) == ELoadState::Loading)
                {
                    std::atomic_wait(&Package->LoadState, State);
                }
                Expected = State;
            }

#if USING(WITH_EDITOR)
            FAssetLoadRecord Entry;
            Entry.Name        = Package->GetName();
            Entry.Source      = EAssetLoadSource::Package;
            Entry.DurationMs  = PlatformTime::ToMilliseconds(PlatformTime::Cycles() - RequestStart);
            Entry.CompletedAt = PlatformTime::Seconds();
            Entry.ThreadId    = (uint32)Threading::GetThreadID();
            Entry.Outcome     = (Expected != ELoadState::Loaded) ? EAssetLoadOutcome::Failed
                              : bJoinedInFlight ? EAssetLoadOutcome::Joined
                              : EAssetLoadOutcome::AlreadyResident;
            FAssetLoadTracker::Get().Record(Entry);
#endif

            return (Expected == ELoadState::Loaded) ? Package : nullptr;
        }
        
        
        bool bSuccess = false;
        const uint64 Start = RequestStart;

        TVector<uint8> FileBinary;
        FBulkRegion    LoadedBulkRegion;
        if (ReadPackageFile(Path, FileBinary, &LoadedBulkRegion))
        {
            Package->SetBulkSource(LoadedBulkRegion, Path);

            Package->CreateLoader(FileBinary);

            FPackageLoader Reader(Package->LoaderBytes, Package);
        
            FPackageHeader PackageHeader;
            Reader << PackageHeader;

            const int64 LoaderSize = Reader.TotalSize();
            auto OffsetInRange = [LoaderSize](int64 Off)
            {
                return Off >= 0 && Off <= LoaderSize;
            };

            if (PackageHeader.Tag != PACKAGE_FILE_TAG)
            {
                LOG_ERROR("LoadPackage: {} is not a valid Lumina package (tag mismatch)", Path);
            }
            else if (PackageHeader.Version > GPackageFileLuminaVersion.FileVersion)
            {
                // Older files load fine since readers branch on the file version, but newer ones cannot be read.
                LOG_ERROR("LoadPackage: {} was saved with engine version {} (current {}); cannot load files from a newer engine", Path, PackageHeader.Version, GPackageFileLuminaVersion.FileVersion);
            }
            else if (!OffsetInRange(PackageHeader.ImportTableOffset) ||
                     !OffsetInRange(PackageHeader.ExportTableOffset) ||
                     !OffsetInRange(PackageHeader.ThumbnailDataOffset))
            {
                LOG_ERROR("LoadPackage: {} has out-of-range header offsets (size={}, import={}, export={}, thumb={})",
                    Path, LoaderSize, PackageHeader.ImportTableOffset, PackageHeader.ExportTableOffset, PackageHeader.ThumbnailDataOffset);
            }
            else
            {
                // Stamped on this reader and kept on the package, so every later reader over these bytes
                // branches for migration the same way.
                Reader.SetFileVersion(PackageHeader.Version);
                Package->LoaderFileVersion = PackageHeader.Version;

                Reader.Seek(PackageHeader.ImportTableOffset);
                Reader << Package->ImportTable;

                Reader.Seek(PackageHeader.ExportTableOffset);
                Reader << Package->ExportTable;

#if USING(WITH_EDITOR)
                // Non-editor saves encode no thumbnail (offset == 0).
                if (PackageHeader.ThumbnailDataOffset != 0)
                {
                    int64 SizeBefore = Reader.Tell();
                    Reader.Seek(PackageHeader.ThumbnailDataOffset);
                    Package->GetPackageThumbnail()->Serialize(Reader);
                    Reader.Seek(SizeBefore);
                }
#endif

                const double DurationMs = PlatformTime::ToMilliseconds(PlatformTime::Cycles() - Start);
                
                bSuccess = true;
                LOG_INFO("Loaded Package: \"{}\" - ( [{}] Exports | [{}] Imports | [{}] Bytes | [{}] ms | Thread: [{}])", Package->GetName(), Package->ExportTable.size(), Package->ImportTable.size(), Package->LoaderBytes->Size, DurationMs, Threading::GetThreadID());

#if USING(WITH_EDITOR)
                FAssetLoadRecord Entry;
                Entry.Name        = Package->GetName();
                Entry.Source      = EAssetLoadSource::Package;
                Entry.Outcome     = EAssetLoadOutcome::Loaded;
                Entry.DurationMs  = DurationMs;
                Entry.CompletedAt = PlatformTime::Seconds();
                Entry.Bytes       = Package->LoaderBytes->Size;
                Entry.Exports     = (uint32)Package->ExportTable.size();
                Entry.Imports     = (uint32)Package->ImportTable.size();
                Entry.ThreadId    = (uint32)Threading::GetThreadID();
                FAssetLoadTracker::Get().Record(Entry);
#endif
            }
        }
        
        Package->LoadState.store(bSuccess ? ELoadState::Loaded : ELoadState::Failed, std::memory_order_release);
        
        std::atomic_notify_all(&Package->LoadState);
        
        Package->AddToRoot();
        return Package;
    }

    namespace
    {
        // Clears then repopulates the export and import tables so back-to-back saves stay consistent.
        bool BuildPackageBytes(CPackage* Package, bool bCooking,
                               TVector<uint8>& OutUncompressed,
                               TVector<uint8>& OutCompressed,
                               CPackage::FBulkRegion& OutBulkRegion)
        {
            (void)Package->FullyLoad();

            Package->ExportTable.clear();
            Package->ImportTable.clear();

            FPackageSaver Writer(OutUncompressed, Package);
            if (bCooking)
            {
                Writer.SetFlag(EArchiverFlags::Cooking);
            }
            if (Package->IsBulkPassthrough())
            {
                Writer.SetFlag(EArchiverFlags::BulkPassthrough);
            }

            FPackageHeader Header;
            Header.Tag = PACKAGE_FILE_TAG;
            Header.Version = GPackageFileLuminaVersion.FileVersion;

            Writer.Seek(sizeof(FPackageHeader));

            FSaveContext SaveContext(Package);
            Package->BuildSaveContext(SaveContext);

            Package->WriteExports(Writer, Header, SaveContext);
            Package->WriteImports(Writer, Header, SaveContext);

            Header.ImportCount = static_cast<int32>(Package->ImportTable.size());
            Header.ExportCount = static_cast<int32>(Package->ExportTable.size());

            // Cook output never carries thumbnails, they're editor-only.
            if (bCooking)
            {
                Header.ThumbnailDataOffset = 0;
            }
            else
            {
#if USING(WITH_EDITOR)
                Header.ThumbnailDataOffset = Writer.Tell();
                Package->GetPackageThumbnail()->Serialize(Writer);
#else
                Header.ThumbnailDataOffset = 0;
#endif
            }

            Writer.Seek(0);
            Writer << Header;

            if (!CompressPackageBinary(OutUncompressed, OutCompressed))
            {
                return false;
            }

            // The bytes come off the source file, so the caller appends the region and trailer itself.
            if (Package->IsBulkPassthrough())
            {
                OutBulkRegion.FileOffset = (int64)OutCompressed.size();
                OutBulkRegion.Size       = Package->GetBulkRegion().Size;
                return true;
            }

            // A package with no bulk data stays byte-identical to what the old saver produced.
            const TVector<uint8>& BulkBytes = Writer.GetBulkBytes();
            OutBulkRegion = CPackage::FBulkRegion{};

            if (!BulkBytes.empty())
            {
                FPackageBulkTrailer Trailer;
                Trailer.Magic      = kBulkTrailerMagic;
                Trailer.Version    = kBulkTrailerVersion;
                Trailer.BulkOffset = (uint64)OutCompressed.size();
                Trailer.BulkSize   = (uint64)BulkBytes.size();

                const size_t ContainerSize = OutCompressed.size();
                OutCompressed.resize(ContainerSize + BulkBytes.size() + sizeof(FPackageBulkTrailer));

                Memory::Memcpy(OutCompressed.data() + ContainerSize, BulkBytes.data(), BulkBytes.size());
                std::memcpy(OutCompressed.data() + ContainerSize + BulkBytes.size(), &Trailer, sizeof(Trailer));

                OutBulkRegion.FileOffset = (int64)Trailer.BulkOffset;
                OutBulkRegion.Size       = (int64)Trailer.BulkSize;
            }

            return true;
        }
    }

    bool CPackage::SavePackage(CPackage* Package, FStringView Path)
    {
        LUMINA_PROFILE_SCOPE();
        LUMINA_MEMORY_SCOPE("Package Save");

        ASSERT(Package != nullptr);

        if (Package->IsTransientPackage())
        {
            LOG_ERROR("SavePackage: refusing to save the engine transient package to {}", Path);
            return false;
        }

        TVector<uint8> FileBinary;
        TVector<uint8> DiskBinary;
        FBulkRegion    NewBulkRegion;

        // Region-relative offsets make the copy legal, and a dirty package may not match what is on disk.
        const bool bSplice = Package->CanSpliceBulkRegion();

        Package->bBulkDataUnresolved = false;
        Package->bBulkPassthrough    = bSplice;

        const bool bBuilt = BuildPackageBytes(Package, /*bCooking*/ false, FileBinary, DiskBinary, NewBulkRegion);

        Package->bBulkPassthrough = false;

        if (!bBuilt)
        {
            LOG_ERROR("Failed to compress package: {}", Path);
            return false;
        }

        // The container describes a layout the source bytes lack, so it must never be written alone.
        if (bSplice)
        {
            // The region streams straight out of the source file, with the locating trailer on the end.
            FPackageBulkTrailer Trailer;
            Trailer.Magic      = kBulkTrailerMagic;
            Trailer.Version    = kBulkTrailerVersion;
            Trailer.BulkOffset = (uint64)NewBulkRegion.FileOffset;
            Trailer.BulkSize   = (uint64)NewBulkRegion.Size;

            const TSpan<const uint8> TrailerBytes(reinterpret_cast<const uint8*>(&Trailer), sizeof(Trailer));

            const bool bSpliced = !Package->bBulkDataUnresolved
                && VFS::AtomicWriteFileSpliced(Path, DiskBinary, Package->BulkSourcePath,
                                               (uint64)Package->BulkRegion.FileOffset,
                                               (uint64)Package->BulkRegion.Size, TrailerBytes);

            if (bSpliced)
            {
                // The spliced region is byte-identical, so an in-flight read gets the same answer either way.
                Package->SetBulkSource(NewBulkRegion, Path);

                Package->CreateLoader(FileBinary);
                Package->ConditionalDropLoader();

                LOG_INFO("Saved Package: \"{}\" - ( [{}] Exports | [{}] Imports | [{:.2f}] KiB container + "
                         "[{:.2f}] KiB bulk spliced from disk )",
                    Package->GetName(),
                    Package->ExportTable.size(),
                    Package->ImportTable.size(),
                    static_cast<double>(DiskBinary.size()) / 1024.0,
                    static_cast<double>(NewBulkRegion.Size) / 1024.0);

                Package->ClearDirty();
                return true;
            }

            // The splice is all-or-nothing, so the rebuild runs from scratch and needs nothing from the source.
            LOG_WARN("SavePackage: cannot splice the bulk region into {} ({}); rebuilding it from memory, "
                     "which reads every streamed payload back in first.", Path,
                     Package->bBulkDataUnresolved ? "an export could not produce a payload the copy would carry"
                                                  : "the file copy failed");

            DiskBinary.clear();
            FileBinary.clear();
            NewBulkRegion = FBulkRegion{};

            Package->bBulkDataUnresolved = false;
            if (!BuildPackageBytes(Package, /*bCooking*/ false, FileBinary, DiskBinary, NewBulkRegion))
            {
                LOG_ERROR("Failed to compress package: {}", Path);
                return false;
            }
        }

        // Writing these would destroy data still intact on disk, and nothing later could undo it.
        if (Package->bBulkDataUnresolved)
        {
            Package->bBulkDataUnresolved = false;
            LOG_ERROR("SavePackage: refusing to write {} -- an export's bulk data could not be read back and "
                      "would be saved as empty. The file on disk is unchanged; see the errors above for "
                      "which payload failed.", Path);
            return false;
        }

        if (!VFS::AtomicWriteFile(Path, DiskBinary))
        {
            // The atomic write failed, so disk is unchanged and the package stays dirty for retry.
            LOG_ERROR("Failed to save package: {}", Path);
            return false;
        }

        // Every ref an export just serialized is relative to THIS region, and the path moves with it.
        Package->SetBulkSource(NewBulkRegion, Path);

        // Dropped again when the save left every export resident, since the bytes are then dead weight.
        Package->CreateLoader(FileBinary);
        Package->ConditionalDropLoader();

        LOG_INFO("Saved Package: \"{}\" - ( [{}] Exports | [{}] Imports | [{:.2f}] KiB on disk, [{:.2f}] KiB uncompressed)",
            Package->GetName(),
            Package->ExportTable.size(),
            Package->ImportTable.size(),
            static_cast<double>(DiskBinary.size()) / 1024.0,
            static_cast<double>(FileBinary.size()) / 1024.0);

        Package->ClearDirty();

        return true;
    }

    bool CPackage::SavePackageForCook(CPackage* Package, TVector<uint8>& OutCompressed)
    {
        LUMINA_PROFILE_SCOPE();
        LUMINA_MEMORY_SCOPE("Package Save");

        ASSERT(Package != nullptr);

        if (Package->IsTransientPackage())
        {
            return false;
        }

        // The region and trailer are part of those bytes, so cooked packages stream like loose ones.
        TVector<uint8> FileBinary;
        FBulkRegion    CookedBulkRegion;

        Package->bBulkDataUnresolved = false;
        if (!BuildPackageBytes(Package, /*bCooking*/ true, FileBinary, OutCompressed, CookedBulkRegion))
        {
            return false;
        }

        // Shipping an emptied payload is a texture the packaged game can never get its mips for.
        if (Package->bBulkDataUnresolved)
        {
            Package->bBulkDataUnresolved = false;
            LOG_ERROR("SavePackageForCook: {} has bulk data that could not be read back; refusing to cook a "
                      "package with the payload emptied out.", Package->GetName());
            OutCompressed.clear();
            return false;
        }

        return true;
    }

    void CPackage::CreateLoader(const TVector<uint8>& FileBinary)
    {
        void* HeapData = Memory::Malloc(FileBinary.size());
        Memory::Memcpy(HeapData, FileBinary.data(), FileBinary.size());

        TSharedPtr<FPackageFileBytes> NewBytes = MakeShared<FPackageFileBytes>(HeapData, (int64)FileBinary.size());

        // Publication only; a reader already walking the old bytes keeps them alive through its own reference.
        FScopeLock Lock(LoaderBytesMutex);
        LoaderBytes = Move(NewBytes);

        // Bytes just written are current format. A caller that parsed an older header overwrites this.
        LoaderFileVersion = GPackageFileLuminaVersion.FileVersion;
    }

    TSharedPtr<FPackageFileBytes> CPackage::AcquireLoaderBytes(int32& OutFileVersion)
    {
        {
            FScopeLock Lock(LoaderBytesMutex);
            if (LoaderBytes)
            {
                OutFileVersion = LoaderFileVersion;
                return LoaderBytes;
            }
        }

        if (!EnsureLoader())
        {
            return nullptr;
        }

        FScopeLock Lock(LoaderBytesMutex);
        OutFileVersion = LoaderFileVersion;
        return LoaderBytes;
    }

    void CPackage::SetBulkSource(const FBulkRegion& Region, FStringView Path)
    {
        FScopeLock Lock(BulkMutex);
        BulkRegion = Region;
        BulkSourcePath.assign(Path.data(), Path.size());

        // Every ref taken against the old layout is now stale, and a reader mid-flight has to notice.
        ++BulkGeneration;
    }

    uint32 CPackage::GetBulkGeneration() const
    {
        FScopeLock Lock(BulkMutex);
        return BulkGeneration;
    }

    bool CPackage::CanSpliceBulkRegion() const
    {
        return !bDirty
            && BulkRegion.IsValid()
            && !BulkSourcePath.empty()
            && VFS::Exists(BulkSourcePath);
    }

    bool CPackage::EnsureLoader()
    {
        {
            FScopeLock Lock(LoaderBytesMutex);
            if (LoaderBytes)
            {
                return true;
            }
        }

        // Mid-rename the name points at a file not yet written, so resolving through it would fail.
        const FFixedString Path = BulkSourcePath.empty() ? GetPackagePath() : BulkSourcePath;
        if (IsTransientPackage() || !VFS::Exists(Path))
        {
            return false;
        }

        TVector<uint8> FileBinary;
        FBulkRegion    ReopenedRegion;
        if (!ReadPackageFile(Path, FileBinary, &ReopenedRegion))
        {
            LOG_ERROR("EnsureLoader: failed to re-read package file {}", Path);
            return false;
        }

        SetBulkSource(ReopenedRegion, Path);

        CreateLoader(FileBinary);

        TSharedPtr<FPackageFileBytes> Bytes;
        {
            FScopeLock Lock(LoaderBytesMutex);
            Bytes = LoaderBytes;
        }

        if (!Bytes)
        {
            return false;
        }

        FPackageLoader Reader(Bytes, this);

        FPackageHeader Header;
        Reader << Header;

        if (Header.Tag != PACKAGE_FILE_TAG)
        {
            LOG_ERROR("EnsureLoader: {} is not a valid Lumina package (tag mismatch)", Path);
            FScopeLock Lock(LoaderBytesMutex);
            LoaderBytes.reset();
            return false;
        }

        // A fresh archive defaults to the current version, so every reader over these bytes is stamped
        // with the file's own version or an older asset is parsed as if it were saved today.
        FScopeLock Lock(LoaderBytesMutex);
        LoaderFileVersion = Header.Version;
        return true;
    }

    void CPackage::ConditionalDropLoader()
    {
        {
            FScopeLock Lock(LoaderBytesMutex);
            if (!LoaderBytes)
            {
                return;
            }
        }

        // A null export pointer is treated conservatively, so bytes are reclaimed only when fully resident.
        for (const FObjectExport& Export : ExportTable)
        {
            const CObject* Obj = Export.Object.Get();
            if (Obj == nullptr || Obj->HasAnyFlag(OF_NeedsLoad))
            {
                return;
            }
        }

        // A reader still walking these bytes holds its own reference, so this only drops the cache.
        FScopeLock Lock(LoaderBytesMutex);
        LoaderBytes.reset();
    }

    void CPackage::BuildSaveContext(FSaveContext& Context)
    {
        TVector<CObject*> ExportObjects;
        ExportObjects.reserve(20);
        GetObjectsWithPackage(this, ExportObjects);

        FSaveReferenceBuilderArchive Builder(&Context);
        for (CObject* Object : ExportObjects)
        {
            Builder << Object;
        }
    }

    void CPackage::WriteImports(FPackageSaver& Ar, FPackageHeader& Header, FSaveContext& SaveContext)
    {
        // Pulls the import order from the saver's map so on-disk indices match what was emitted.
        Ar.PopulateImportTable(ImportTable);

        Header.ImportTableOffset = Ar.Tell();
        Ar << ImportTable;
    }

    void CPackage::WriteExports(FPackageSaver& Ar, FPackageHeader& Header, FSaveContext& SaveContext)
    {
        Header.ObjectDataOffset = Ar.Tell();

        for (CObject* Export : SaveContext.Exports)
        {
            Export->LoaderIndex = FObjectPackageIndex::FromExport((int32)ExportTable.size()).GetRaw();
            ExportTable.emplace_back(Export);
        }

        for (FObjectExport& Export : ExportTable)
        {
            ASSERT(Export.Object.Get() != nullptr);

            // PreSave can pull payloads off disk, and that must not land between the offset and the data.
            Export.Object.Get()->PreSave();

            Export.Offset = Ar.Tell();

            Export.Object.Get()->Serialize(Ar);
            
            Export.Size = Ar.Tell() - Export.Offset;
            
        }
        
        Header.ExportTableOffset = Ar.Tell();
        Ar << ExportTable;
    }

    void CPackage::SerializeObject(CObject* Object)
    {
        LUMINA_PROFILE_SCOPE();
        if (!Object || !Object->HasAnyFlag(OF_NeedsLoad))
        {
            return;
        }

        CPackage* ObjectPackage = Object->GetPackage();
        if (ObjectPackage != this)
        {
            ObjectPackage->SerializeObject(Object);
            return;
        }

        // Whoever claims it reads it; a second thread waits here rather than racing the same export, and
        // this thread re-entering its own load takes the partial object so a reference cycle resolves.
        const EClaimResult Claim = ClaimObjectForLoad(Object);
        if (Claim != EClaimResult::Owned)
        {
            return;
        }

        // Released on every path out, or a waiter would sit here forever.
        struct FClaimScope
        {
            CObject* Object;
            ~FClaimScope() { ReleaseObjectLoad(Object); }
        } ClaimScope{ Object };

        int32 FoundLoaderIndex = FObjectPackageIndex(Object->LoaderIndex).GetArrayIndex();

        if (FoundLoaderIndex < 0 || Cmp::GreaterEqual(FoundLoaderIndex, ExportTable.size()))
        {
            LOG_ERROR("Invalid loader index {} for object {}", FoundLoaderIndex, Object->GetName());
            Object->ClearFlags(OF_Loading);
            return;
        }

        FObjectExport& Export = ExportTable[FoundLoaderIndex];

        const int64 DataPos = Export.Offset;
        const int64 ExpectedSize = Export.Size;

        if (DataPos < 0 || ExpectedSize <= 0)
        {
            LOG_ERROR("Invalid export data for object {}. Offset: {}, Size: {}", Object->GetName().ToString(), DataPos, ExpectedSize);
            Object->ClearFlags(OF_Loading);
            return;
        }

        // The cached file bytes are dropped once a package is fully resident; re-open them on demand.
        int32 FileVersion = 0;
        TSharedPtr<FPackageFileBytes> Bytes = AcquireLoaderBytes(FileVersion);

        if (!Bytes)
        {
            LOG_ERROR("No loader set for package {}", GetName().ToString());
            Object->ClearFlags(OF_Loading);
            return;
        }

        // This reader's own cursor, and its own reference to the bytes, so a concurrent load of another
        // export cannot move it and a concurrent drop of the cache cannot free it.
        FPackageLoader Reader(Bytes, this);
        Reader.SetFileVersion(FileVersion);
        Reader.Seek(DataPos);

        Object->PreLoad();

        Object->Serialize(Reader);

        const int64 ActualSize = Reader.Tell() - DataPos;

        if (Reader.HasError())
        {
            LOG_ERROR("Corrupt read loading '{}' from package '{}'; the object is left at its defaults.",
                      Object->GetName().ToString(), GetName().ToString());
        }

        if (ActualSize != ExpectedSize)
        {
            LOG_WARN("Mismatched size when loading object {}: expected {}, got {}", Object->GetName().ToString(), ExpectedSize, ActualSize);
        }

        // This is what lets serialize and PostLoad split into phases.
        Object->ClearFlags(OF_NeedsLoad | OF_Loading);
        Object->SetFlag(OF_WasLoaded | OF_NeedsPostLoad);

        // Reclaim the cached file bytes once the package is fully resident.
        ConditionalDropLoader();
    }

    void CPackage::PostLoadObject(CObject* Object)
    {
        if (Object && Object->HasAnyFlag(OF_NeedsPostLoad))
        {
            Object->ClearFlags(OF_NeedsPostLoad);
            Object->PostLoad();
        }
    }

    void CPackage::LoadObject(CObject* Object)
    {
        SerializeObject(Object);

        // Every other caller gets the legacy serialize-then-PostLoad behavior.
        if (!ShouldDeferPostLoad(Object))
        {
            PostLoadObject(Object);
        }
    }

    CObject* CPackage::CreateExportShell(int32 ExportIndex)
    {
        FObjectExport& Export = ExportTable[ExportIndex];

        CObject* Object = FindObjectImpl(Export.ObjectGUID);

        if (Object == nullptr)
        {
            CClass* ObjectClass = FindObject<CClass>(Export.ClassName);
            if (ObjectClass == nullptr)
            {
                LOG_ERROR("CreateExportShell: class '{}' for export '{}' in package '{}' could not be resolved", Export.ClassName, Export.ObjectName, GetName());
                return nullptr;
            }

            Object = NewObject(ObjectClass, this, Export.ObjectName, Export.ObjectGUID);
            Object->SetFlag(OF_NeedsLoad);

            if (Object->IsAsset())
            {
                Object->SetFlag(OF_Public);
            }
        }

        Object->LoaderIndex = FObjectPackageIndex::FromExport(ExportIndex).GetRaw();
        Export.Object = Object;

        return Object;
    }

    void CPackage::CreateExportShells()
    {
        for (int32 i = 0; i < (int32)ExportTable.size(); ++i)
        {
            CreateExportShell(i);
        }
    }

    void CPackage::SerializeExports()
    {
        for (FObjectExport& Export : ExportTable)
        {
            if (CObject* Object = Export.Object.Get())
            {
                SerializeObject(Object);
            }
        }
    }

    void CPackage::PostLoadExports()
    {
        for (FObjectExport& Export : ExportTable)
        {
            if (CObject* Object = Export.Object.Get())
            {
                PostLoadObject(Object);
            }
        }
    }

    CObject* CPackage::LoadObject(const FGuid& GUID)
    {
        for (size_t i = 0; i < ExportTable.size(); ++i)
        {
            FObjectExport& Export = ExportTable[i];

            if (Export.ObjectGUID == GUID)
            {
                CObject* Object = CreateExportShell(static_cast<int32>(i));
                if (Object == nullptr)
                {
                    return nullptr;
                }

                LoadObject(Object);

                return Object;
            }
        }

        return nullptr;
    }

    CObject* CPackage::LoadObjectByName(const FName& Name)
    {
        for (size_t i = 0; i < ExportTable.size(); ++i)
        {
            FObjectExport& Export = ExportTable[i];

            if (Export.ObjectName == Name)
            {
                CObject* Object = CreateExportShell(static_cast<int32>(i));
                if (Object == nullptr)
                {
                    return nullptr;
                }

                LoadObject(Object);

                return Object;
            }
        }

        return nullptr;
    }

    CObject* CPackage::LoadAssetGraph(const FGuid& RootGUID)
    {
        LUMINA_PROFILE_SCOPE();
        
        static FMutex GraphLoadMutex;
        FScopeLock GraphLock(GraphLoadMutex);

        FAssetRegistry& Registry = FAssetRegistry::Get();

        if (Registry.GetAssetByGUID(RootGUID) == nullptr)
        {
            // Not a registered asset (transient), take the inline path.
            return StaticLoadObject(RootGUID);
        }
        
        THashMap<FGuid, int32>      GuidToNode;
        TVector<FFixedString>       NodePaths;
        TVector<THashSet<int32>>    NodeDeps;

        auto GetOrAddNode = [&](const FGuid& Guid, const FAssetData* Data) -> int32
        {
            auto It = GuidToNode.find(Guid);
            if (It != GuidToNode.end())
            {
                return It->second;
            }
            const int32 Index = (int32)NodePaths.size();
            GuidToNode.emplace(Guid, Index);
            NodePaths.push_back(Data->Path);
            NodeDeps.emplace_back();
            return Index;
        };

        THashSet<FGuid> Visited;
        TVector<FGuid>  Queue;
        Queue.push_back(RootGUID);

        while (!Queue.empty())
        {
            const FGuid Guid = Queue.back();
            Queue.pop_back();

            if (!Visited.insert(Guid).second)
            {
                continue;
            }

            const FAssetData* Data = Registry.GetAssetByGUID(Guid);
            if (Data == nullptr)
            {
                continue; // unresolvable ref; the inline fallback during serialize handles it
            }

            const int32 NodeIdx = GetOrAddNode(Guid, Data);

            for (const FAssetDependency& Dep : Data->Dependencies)
            {
                // Only eagerly load the always-resident graph; Soft/Script/EditorOnly/Generated stream lazily.
                if (Dep.Type != EDependencyType::Hard && Dep.Type != EDependencyType::Owned)
                {
                    continue;
                }

                if (const FAssetData* DepData = Registry.GetAssetByGUID(Dep.TargetGUID))
                {
                    const int32 DepIdx = GetOrAddNode(Dep.TargetGUID, DepData);
                    if (DepIdx != NodeIdx)
                    {
                        NodeDeps[NodeIdx].insert(DepIdx);
                    }
                }
                Queue.push_back(Dep.TargetGUID);
            }
        }

        const int32 NumNodes = (int32)NodePaths.size();

        TVector<CPackage*> Packages;
        Packages.resize(NumNodes, nullptr);
        Task::ParallelFor((uint32)NumNodes, [&](uint32 i)
        {
            Packages[i] = LoadPackage(NodePaths[i]);
        }, 1);

        // Need to create the shells to assets actually have a live pointer reference.
        Task::ParallelFor((uint32)NumNodes, [&](uint32 i)
        {
            if (Packages[i])
            {
                Packages[i]->CreateExportShells();
            }
        }, 1);

        THashSet<CPackage*> ClosureSet;
        ClosureSet.reserve(NumNodes);
        for (CPackage* P : Packages)
        {
            if (P)
            {
                ClosureSet.insert(P);
            }
        }

        GGraphClosure = &ClosureSet;

        Task::ParallelFor((uint32)NumNodes, [&](uint32 i)
        {
            if (Packages[i])
            {
                // Scoped to this worker's stack so nested in-closure loads also defer their PostLoad.
                GtDeferPostLoad = true;
                Packages[i]->SerializeExports();
                GtDeferPostLoad = false;
            }
        }, 1);

        GGraphClosure = nullptr;
        
        TVector<uint8> Processed(NumNodes, 0);
        TVector<int32> Wave;
        int32          Remaining = NumNodes;

        while (Remaining > 0)
        {
            Wave.clear();
            for (int32 i = 0; i < NumNodes; ++i)
            {
                if (Processed[i])
                {
                    continue;
                }
                bool bReady = true;
                for (int32 Dep : NodeDeps[i])
                {
                    if (!Processed[Dep])
                    {
                        bReady = false;
                        break;
                    }
                }
                if (bReady)
                {
                    Wave.push_back(i);
                }
            }

            if (Wave.empty())
            {
                // A dependency cycle breaks by taking them all at once, and the per-asset guards order the rest.
                for (int32 i = 0; i < NumNodes; ++i)
                {
                    if (!Processed[i])
                    {
                        Wave.push_back(i);
                    }
                }
            }

            Task::ParallelFor((uint32)Wave.size(), [&](uint32 k)
            {
                const int32 Idx = Wave[k];
                if (Packages[Idx])
                {
                    Packages[Idx]->PostLoadExports();
                }
            }, 1);

            for (int32 Idx : Wave)
            {
                Processed[Idx] = 1;
                --Remaining;
            }
        }

        return FindObjectImpl(RootGUID);
    }

    bool CPackage::FullyLoad()
    {
        bool bAllOk = true;
        for (const FObjectExport& Export : ExportTable)
        {
            if (LoadObject(Export.ObjectGUID) == nullptr)
            {
                bAllOk = false;
            }
        }

        return bAllOk;
    }

    CObject* CPackage::FindObjectInPackage(const FName& Name)
    {
        for (const FObjectExport& Export : ExportTable)
        {
            if (Export.ObjectName == Name)
            {
                return Export.Object.Get();
            }
        }

        return nullptr;
    }

    CObject* CPackage::IndexToObject(const FObjectPackageIndex& Index)
    {
        if (Index.IsNull())
        {
            return nullptr;
        }
        
        if (Index.IsImport())
        {
            size_t ArrayIndex = Index.GetArrayIndex();
            if (ArrayIndex >= ImportTable.size())
            {
                LOG_WARN("Failed to find an object in the import table {}", ArrayIndex);
                return nullptr;
            }

            FObjectImport& Import = ImportTable[ArrayIndex];
            Import.Object = Lumina::LoadObject<CObject>(Import.ObjectGUID);
            
            return ImportTable[ArrayIndex].Object.Get();
        }

        if (Index.IsExport())
        {
            size_t ArrayIndex = Index.GetArrayIndex();
            if (ArrayIndex >= ExportTable.size())
            {
                LOG_WARN("Failed to find an object in the export table {}", ArrayIndex);
                return nullptr;
            }

            return LoadObject(ExportTable[ArrayIndex].ObjectGUID);
        }
        
        return nullptr;
    }

#if USING(WITH_EDITOR)
    FPackageThumbnail* CPackage::GetPackageThumbnail()
    {
        FScopeLock Lock(ThumbnailMutex);

        if (PackageThumbnail == nullptr)
        {
            PackageThumbnail = MakeUnique<FPackageThumbnail>();
        }

        return PackageThumbnail.get();
    }
#endif

    FFixedString CPackage::GetPackagePath() const
    {
        FFixedString Path(GetName().c_str(), GetName().Length());
        AddPackageExt(Path);
        
        return Path;
    }
}
