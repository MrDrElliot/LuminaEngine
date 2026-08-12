#include "RuntimePCH.h"
#include "Package.h"
#include <utility>
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Object/Archive/ObjectReferenceReplacerArchive.h"
#include "Core/Profiler/Profile.h"
#include "Core/Serialization/Package/PackageLoader.h"
#include "Core/Serialization/Package/PackageSaver.h"
#include "FileSystem/FileSystem.h"
#include "Paths/Paths.h"
#include "TaskSystem/TaskSystem.h"
#include "Thumbnail/PackageThumbnail.h"
#include "miniz.h"
#include "Log/Log.h"


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

        // Bulk region trailer. A package with bulk data is [compressed container][raw bulk][trailer], and
        // the trailer is the only fixed-position thing in the file, so it is what a reader finds first.
        // Putting the region's location at the END (rather than in FPackageHeader) is what keeps the header
        // wire-compatible: the header is inside the compressed container and cannot describe the file's
        // own layout without a two-pass save.
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

        // Below this, a package is read whole and its trailer parsed in memory: one IO instead of two.
        // Above it, the tail is read first so a file whose bulk region dwarfs its container (exactly what a
        // 4K texture looks like) never pulls the payload it is trying to avoid pulling.
        constexpr uint64 kWholeFileReadLimit = 1u * 1024 * 1024;

        // Reads the trailer out of an in-memory tail slice. bytes must be the LAST bytes of the file, and
        // FileSize the file's true length -- the offsets are validated against it.
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

            // A malformed or truncated trailer must not be trusted into a read: the region has to sit
            // entirely between the container and the trailer itself.
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

            // Each chunk inflates into a disjoint output slice, so the chunks are independent. A big
            // multi-chunk package (e.g. a world or texture atlas) is the dominant load cost, so fan the
            // inflate across workers. Single-chunk packages (the common small asset) stay inline to avoid
            // a pointless task hop and any nested-parallelism cost on the registry-discovery path.
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
            // Distinguish the chunked container by its leading magic; everything else is legacy
            // single-stream (whose first bytes are an uncompressed-size that can't collide with the magic).
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

        // Small packages (the overwhelming majority -- materials, prefabs, settings) keep the single-read
        // path they have always had; splitting them into a tail read plus a body read would double the IO
        // count on a world load to save nothing.
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
                // Trim to the container: the bulk bytes and trailer are not part of the deflate stream.
                RawBinary.resize((size_t)Region.FileOffset);
                if (OutBulkRegion)
                {
                    *OutBulkRegion = Region;
                }
            }

            return DecompressPackageBinary(RawBinary, OutBinary);
        }

        // Large file: find out whether there is a bulk region before reading anything big.
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

    bool CPackage::ReadBulkData(const FBulkDataRef& Ref, TVector<uint8>& OutBytes) const
    {
        OutBytes.clear();

        if (!Ref.IsValid())
        {
            return false;
        }

        if (!BulkRegion.IsValid() || Ref.Offset < 0 || Ref.Size > BulkRegion.Size - Ref.Offset)
        {
            LOG_ERROR("ReadBulkData: ref (offset={}, size={}) is outside package {}'s bulk region (size={})",
                Ref.Offset, Ref.Size, GetName(), BulkRegion.Size);
            return false;
        }

        const FFixedString Path = GetPackagePath();
        if (!VFS::ReadFileRange(OutBytes, Path, (uint64)(BulkRegion.FileOffset + Ref.Offset), (uint64)Ref.Size))
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

        return true;
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
        // In-memory rename only; caller (RenamePackage) handles disk. Exported objects survive so live refs stay valid.
        // FName::ToString() returns by value; bind to locals so the FStringViews aren't dangling.
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

        // Mark first so FindObject (name + GUID) stops resolving it immediately -- a deleted asset is
        // unreachable by identity even while its husk is torn down below.
        PackageToDestroy->SetFlag(OF_MarkedDestroy);

        // Synchronous teardown.
        {
            // Pinned for the duration of the sweep. Nulling the last reference to an export frees it on
            // the spot, and a primary asset's destructor releases the sub-object exports it owns (particle
            // emitters, graph nodes) -- which are entries in this same list. Without the pin those
            // siblings die mid-loop and the next iteration reads a freed vtable.
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

        // Pins are gone: whatever the sweep orphaned has been freed, so the list is rebuilt from what is
        // still alive rather than reusing pointers that may now dangle.
        TVector<CObject*> SurvivingExports;
        SurvivingExports.reserve(20);
        GetObjectsWithPackage(PackageToDestroy, SurvivingExports);

        // Free the now-unreferenced assets. ConditionalBeginDestroy is a no-op for any still held by a
        // non-reflected strong ref (e.g. an open editor, which closes on its own deferred queue and releases).
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

        // Always rename-then-resave: in-place export-table patching is unsafe (FName length-prefix shifts offsets).
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
        // Parent-dir rename: file already at NewPath on disk, just update in-memory identity.
        if (OldPath == NewPath)
        {
            return;
        }

        FFixedString OldObjectName = SanitizeObjectName(OldPath);
        if (CPackage* Package = FindObject<CPackage>(OldObjectName))
        {
            FFixedString NewObjectName = SanitizeObjectName(NewPath);
            Package->Rename(NewObjectName, nullptr);
        }
    }

    CPackage* CPackage::LoadPackage(FStringView Path)
    {
        LUMINA_PROFILE_SCOPE();
        
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
        
        ELoadState Expected = ELoadState::Unloaded;
        bool bIsLoaderThread = Package->LoadState.compare_exchange_strong(
            Expected, 
            ELoadState::Loading,
            std::memory_order_acquire,
            std::memory_order_acquire);
        
        if (!bIsLoaderThread)
        {
            if (Expected == ELoadState::Loading)
            {
                ELoadState State;
                while ((State = Package->LoadState.load(std::memory_order_acquire)) == ELoadState::Loading)
                {
                    std::atomic_wait(&Package->LoadState, State);
                }
                Expected = State;
            }
        
            return (Expected == ELoadState::Loaded) ? Package : nullptr;
        }
        
        
        bool bSuccess = false;
        auto Start = std::chrono::high_resolution_clock::now();

        TVector<uint8> FileBinary;
        if (ReadPackageFile(Path, FileBinary, &Package->BulkRegion))
        {
            Package->CreateLoader(FileBinary);
        
            FPackageLoader& Reader = *static_cast<FPackageLoader*>(Package->Loader.get());
        
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
                // Older files load fine, readers branch on Ar.GetFileVersion() to migrate.
                // Newer files we genuinely can't read.
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
                // Stamp source version so per-type Serialize can branch for migration.
                Reader.SetFileVersion(PackageHeader.Version);

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

                auto End = std::chrono::high_resolution_clock::now();
                auto Duration = std::chrono::duration_cast<std::chrono::milliseconds>(End - Start);
                
                bSuccess = true;
                LOG_INFO("Loaded Package: \"{}\" - ( [{}] Exports | [{}] Imports | [{}] Bytes | [{}] ms | Thread: [{}])", Package->GetName(), Package->ExportTable.size(), Package->ImportTable.size(), Package->Loader->TotalSize(), Duration, Threading::GetThreadID());
            }
        }
        
        Package->LoadState.store(bSuccess ? ELoadState::Loaded : ELoadState::Failed, std::memory_order_release);
        
        std::atomic_notify_all(&Package->LoadState);
        
        Package->AddToRoot();
        return Package;
    }

    namespace
    {
        // Build the on-disk bytes for a package (shared by editor and cook save).
        // Clears then repopulates Package->Export/ImportTable so back-to-back saves stay consistent.
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

            // Bulk region rides after the container, raw. Emitted only when an export actually wrote
            // something, so a package with no bulk data is byte-identical to what the old saver produced
            // and stays readable by the whole-file path.
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

        ASSERT(Package != nullptr);

        if (Package->IsTransientPackage())
        {
            LOG_ERROR("SavePackage: refusing to save the engine transient package to {}", Path);
            return false;
        }

        TVector<uint8> FileBinary;
        TVector<uint8> DiskBinary;
        FBulkRegion    NewBulkRegion;
        if (!BuildPackageBytes(Package, /*bCooking*/ false, FileBinary, DiskBinary, NewBulkRegion))
        {
            LOG_ERROR("Failed to compress package: {}", Path);
            return false;
        }

        if (!VFS::AtomicWriteFile(Path, DiskBinary))
        {
            // Atomic write failed: disk unchanged, package stays dirty for retry.
            LOG_ERROR("Failed to save package: {}", Path);
            return false;
        }

        // Only after the write commits: every FBulkDataRef an export just serialized is relative to THIS
        // region, and a reader that used the old one would read the previous save's bytes.
        Package->BulkRegion = NewBulkRegion;

        // Refresh loader only after disk commit so it can't point at uncommitted bytes (a partially-resident
        // package still needs it for lazy loads at the new offsets). Drop it again immediately if the save
        // left every export resident -- then the bytes are dead weight, re-read on demand if ever needed.
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

        ASSERT(Package != nullptr);

        if (Package->IsTransientPackage())
        {
            return false;
        }

        // The cooker writes OutCompressed straight into the pak, and the bulk region + trailer are part of
        // those bytes, so cooked packages stream exactly like loose ones.
        TVector<uint8> FileBinary;
        FBulkRegion    CookedBulkRegion;
        return BuildPackageBytes(Package, /*bCooking*/ true, FileBinary, OutCompressed, CookedBulkRegion);
    }

    void CPackage::CreateLoader(const TVector<uint8>& FileBinary)
    {
        void* HeapData = Memory::Malloc(FileBinary.size());
        Memory::Memcpy(HeapData, FileBinary.data(), FileBinary.size());
        Loader = MakeUnique<FPackageLoader>(HeapData, FileBinary.size(), this);
    }

    bool CPackage::EnsureLoader()
    {
        if (Loader)
        {
            return true;
        }

        // In-memory (transient / never-saved) packages have no backing file to re-read; their objects are
        // resident anyway, so this path is only ever hit for disk-backed packages whose bytes we dropped.
        const FFixedString Path = GetPackagePath();
        if (IsTransientPackage() || !VFS::Exists(Path))
        {
            return false;
        }

        TVector<uint8> FileBinary;
        if (!ReadPackageFile(Path, FileBinary, &BulkRegion))
        {
            LOG_ERROR("EnsureLoader: failed to re-read package file {}", Path);
            return false;
        }

        CreateLoader(FileBinary);
        return Loader != nullptr;
    }

    void CPackage::ConditionalDropLoader()
    {
        // Never free the buffer out from under an in-flight (possibly nested) load.
        if (!Loader || ActiveLoadDepth > 0)
        {
            return;
        }

        // Keep the cached bytes while any export still needs them. A null/stale export weak-ptr is treated
        // conservatively as "might load again" (we can't tell a never-requested export from a freed one),
        // so we only reclaim once the whole package is resident -- exactly when the buffer is dead weight.
        for (const FObjectExport& Export : ExportTable)
        {
            const CObject* Obj = Export.Object.Get();
            if (Obj == nullptr || Obj->HasAnyFlag(OF_NeedsLoad))
            {
                return;
            }
        }

        Loader.reset();
    }

    FPackageLoader* CPackage::GetLoader() const
    {
        return (FPackageLoader*)Loader.get();
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
        // Must run AFTER WriteExports, pulls the import order from the saver's ObjectToIndexMap
        // so on-disk indices match what was emitted in export data.
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

            // Before Tell(): PreSave can pull bulk payloads off disk, and anything it does must not land in
            // the export stream between the recorded offset and the object's own data.
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
        if (!Object || !Object->HasAnyFlag(OF_NeedsLoad) || Object->HasAnyFlag(OF_Loading))
        {
            return;
        }

        Object->SetFlag(OF_Loading);

        CPackage* ObjectPackage = Object->GetPackage();

        if (ObjectPackage != this)
        {
            ObjectPackage->SerializeObject(Object);
            return;
        }

        int32 FoundLoaderIndex = FObjectPackageIndex(Object->LoaderIndex).GetArrayIndex();

        if (FoundLoaderIndex < 0 || std::cmp_greater_equal(FoundLoaderIndex, ExportTable.size()))
        {
            LOG_ERROR("Invalid loader index {} for object {}", FoundLoaderIndex, Object->GetName());
            Object->ClearFlags(OF_Loading);
            return;
        }

        FObjectExport& Export = ExportTable[FoundLoaderIndex];

        // The cached file bytes are dropped once a package is fully resident; re-open them on demand.
        if (!Loader)
        {
            EnsureLoader();
        }

        if (!Loader)
        {
            LOG_ERROR("No loader set for package {}", GetName().ToString());
            Object->ClearFlags(OF_Loading);
            return;
        }

        const int64 SavedPos = Loader->Tell();
        const int64 DataPos = Export.Offset;
        const int64 ExpectedSize = Export.Size;

        if (DataPos < 0 || ExpectedSize <= 0)
        {
            LOG_ERROR("Invalid export data for object {}. Offset: {}, Size: {}", Object->GetName().ToString(), DataPos, ExpectedSize);
            Object->ClearFlags(OF_Loading);
            return;
        }

        // Guard the buffer against being dropped by a nested IndexToObject load triggered inside Serialize.
        ++ActiveLoadDepth;

        Loader->Seek(DataPos);

        Object->PreLoad();

        Object->Serialize(*Loader);

        const int64 ActualSize = Loader->Tell() - DataPos;

        if (ActualSize != ExpectedSize)
        {
            LOG_WARN("Mismatched size when loading object {}: expected {}, got {}", Object->GetName().ToString(), ExpectedSize, ActualSize);
        }

        // Data is resident but PostLoad is still owed; the caller (or the graph loader's leaf-first pass)
        // runs it via PostLoadObject. This is what lets serialize and PostLoad split into phases.
        Object->ClearFlags(OF_NeedsLoad | OF_Loading);
        Object->SetFlag(OF_WasLoaded | OF_NeedsPostLoad);

        Loader->Seek(SavedPos);

        --ActiveLoadDepth;

        // Reclaim the cached file bytes once the outermost load leaves the package fully resident.
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

        // The phased graph loader defers PostLoad for in-closure objects to its ordered leaf-first pass.
        // Every other caller (and out-of-closure refs) gets the legacy serialize-then-PostLoad behavior.
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
                // Scope the defer flag to this worker's call stack so nested in-closure loads (same-package
                // export refs reached inside Serialize) also defer their PostLoad to Phase D.
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
                // Dependency cycle among the survivors (e.g. a material instance pair): break it by taking
                // them all at once; the per-asset PostLoad guards resolve the intra-cycle order.
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
