#pragma once

#include "Lumina.h"
#include "Assets/AssetRegistry/AssetData.h"
#include "Core/Delegates/Delegate.h"
#include "Core/Object/Class.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/Package/Thumbnail/PackageThumbnail.h"
#include "Core/Serialization/Package/PackageLoader.h"
#include "Core/Serialization/Package/PackageSaver.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    struct FPackageThumbnail;
    class FSaveContext;
    class FPackageLoader;
}

#define PACKAGE_FILE_TAG 0x9E2A83C1

namespace Lumina
{
    DECLARE_MULTICAST_DELEGATE(FPackageDestroyedDelegate, FName);
    
    struct FObjectExport
    {
        FObjectExport() = default;
        RUNTIME_API FObjectExport(CObject* InObject);

        FGuid ObjectGUID;
        FName ObjectName;
        FName ClassName;
        int64 Offset;
        int64 Size;
        TWeakObjectPtr<CObject> Object;

        FORCEINLINE friend FArchive& operator << (FArchive& Ar, FObjectExport& Data)
        {
            Ar << Data.ObjectGUID;
            Ar << Data.ObjectName;
            Ar << Data.ClassName;
            Ar << Data.Offset;
            Ar << Data.Size;
            
            return Ar;
        }
    };

    struct FObjectImport
    {
        FObjectImport() = default;
        FObjectImport(CObject* InObject);
        FObjectImport(const FGuid& InGUID, EDependencyType InType)
            : ObjectGUID(InGUID), Type(InType) {}

        FGuid ObjectGUID;
        /** Resolved after import is loaded. */
        TWeakObjectPtr<CObject> Object;

        /** Cook-graph edge classification, Soft for an FSoftObjectPath and Hard for a direct pointer. */
        EDependencyType Type = EDependencyType::Hard;

        FORCEINLINE friend FArchive& operator << (FArchive& Ar, FObjectImport& Data)
        {
            Ar << Data.ObjectGUID;
            uint8 T = static_cast<uint8>(Data.Type);
            Ar << T;
            if (Ar.IsReading())
            {
                Data.Type = static_cast<EDependencyType>(T);
            }
            return Ar;
        }
    };
    

    struct FPackageHeader
    {
        uint32 Tag;
        int32 Version;
        int64 ImportTableOffset;
        int32 ImportCount;
        int64 ExportTableOffset;
        int32 ExportCount;
        int64 ObjectDataOffset;
        int64 ThumbnailDataOffset;

        friend FArchive& operator << (FArchive& Ar, FPackageHeader& Data)
        {
            Ar << Data.Tag;
            Ar << Data.Version;
            Ar << Data.ImportTableOffset;
            Ar << Data.ImportCount;
            Ar << Data.ExportTableOffset;
            Ar << Data.ExportCount;
            Ar << Data.ObjectDataOffset;
            Ar << Data.ThumbnailDataOffset;

            return Ar;
        }
    };
    static_assert(std::is_standard_layout_v<FPackageHeader>, "FPackageHeader must only contain trivial data members");
    static_assert(std::is_trivially_copyable_v<FPackageHeader>, "FPackageHeader must only contain trivial data members");
    
    /** Negative is an import, positive an export, 0 null, encoded as -(i+1) and i+1. */
    struct FObjectPackageIndex
    {
    public:

        FObjectPackageIndex() : Index(0) {}

        explicit FObjectPackageIndex(int32 InIndex) : Index(InIndex) {}

        static FObjectPackageIndex FromImport(int32 ImportArrayIndex)
        {
            return FObjectPackageIndex(-(ImportArrayIndex + 1));
        }

        static FObjectPackageIndex FromExport(int32 ExportArrayIndex)
        {
            return FObjectPackageIndex(ExportArrayIndex + 1);
        }

        bool IsNull() const
        {
            return Index == 0;
        }

        bool IsImport() const
        {
            return Index < 0;
        }

        bool IsExport() const
        {
            return Index > 0;
        }

        int32 GetRaw() const
        {
            return Index;
        }

        int32 GetArrayIndex() const
        {
            if (IsNull())
            {
                return INDEX_NONE;
            }

            return IsExport() ? (Index - 1) : (-Index - 1);
        }

        bool operator==(const FObjectPackageIndex& Other) const { return Index == Other.Index; }
        bool operator!=(const FObjectPackageIndex& Other) const { return Index != Other.Index; }

        FORCEINLINE friend FArchive& operator << (FArchive& Ar, FObjectPackageIndex& Data)
        {
            Ar << Data.Index;
            
            return Ar;
        }
        
    private:
        
        int32 Index;
    };

    class CPackage : public CObject
    {
    public:

        DECLARE_CLASS(Lumina, CPackage, CObject, "" /** Intentionally empty */, RUNTIME_API)
        DEFINE_CLASS_FACTORY(CPackage)
        
        enum class ELoadState : uint8
        {
            Unloaded,
            Loading,
            Loaded,
            Failed
        };

        void OnDestroy() override;
        bool Rename(const FName& NewName, CPackage* NewPackage) override;
        
        RUNTIME_API static CPackage* CreatePackage(FStringView Path);

        /** Engine-wide in-memory package for runtime-only objects (engine primitives, default materials). Never saved. */
        RUNTIME_API static CPackage* GetTransientPackage();

        RUNTIME_API bool IsTransientPackage() const;

        RUNTIME_API static bool DestroyPackage(FStringView Path);

        RUNTIME_API static bool DestroyPackage(CPackage* PackageToDestroy);

        RUNTIME_API static CPackage* FindPackageByPath(FStringView Path);

        /** Atomic in-memory and on-disk rename, crash-safe by write-then-remove, false on collision or IO error. */
        RUNTIME_API NODISCARD static bool RenamePackage(FStringView OldPath, FStringView NewPath);

        /** Update in-memory identity when a parent dir was renamed externally; file name unchanged, no disk I/O. */
        RUNTIME_API static void OnPackageMovedExternally(FStringView OldPath, FStringView NewPath);


        /** Idempotent. Returns shells marked OF_NeedsLoad; objects are not yet serialized. */
        RUNTIME_API static CPackage* LoadPackage(FStringView Path);

        /** Parallel phased load of RootGUID and its hard closure, falling back to the inline load when unregistered. */
        RUNTIME_API static CObject* LoadAssetGraph(const FGuid& RootGUID);

        RUNTIME_API static bool SavePackage(CPackage* Package, FStringView Path);

        /** Saves to compressed bytes for the cooker, stripping EditorOnly properties and thumbnails. */
        RUNTIME_API NODISCARD static bool SavePackageForCook(CPackage* Package, TVector<uint8>& OutCompressed);

        /** Where a package's bulk region sits in its file, file-absolute, with the refs inside exports relative to it. */
        struct FBulkRegion
        {
            int64 FileOffset = 0;
            int64 Size       = 0;

            bool IsValid() const { return Size > 0; }
        };

        /** Reads and decompresses the package container, leaving the bulk region on disk and merely locating it. */
        RUNTIME_API static bool ReadPackageFile(FStringView Path, TVector<uint8>& OutBinary, FBulkRegion* OutBulkRegion = nullptr);

        /** One ranged read of a bulk payload, uncached so the caller owns the bytes, and safe from a worker thread. */
        RUNTIME_API NODISCARD bool ReadBulkData(const FBulkDataRef& Ref, TVector<uint8>& OutBytes, uint32 ExpectedGeneration = 0) const;

        /** Bumped by every save, so a caller holding refs it captured earlier can tell they went stale. */
        RUNTIME_API NODISCARD uint32 GetBulkGeneration() const;

        RUNTIME_API NODISCARD const FBulkRegion& GetBulkRegion() const { return BulkRegion; }

        /** Reports a payload that could not be read back, which makes SavePackage refuse rather than write it empty. */
        RUNTIME_API void FlagUnresolvedBulkData() { bBulkDataUnresolved = true; }

        /** True while a save copies this bulk region across verbatim, where a PreSave that reads payloads back costs the whole saving. */
        RUNTIME_API NODISCARD bool IsBulkPassthrough() const { return bBulkPassthrough; }

        void CreateLoader(const TVector<uint8>& FileBinary);

        // The cached bytes and the version to stamp a reader with, re-read on demand, null for a transient package.
        TSharedPtr<FPackageFileBytes> AcquireLoaderBytes(int32& OutFileVersion);

        RUNTIME_API void BuildSaveContext(FSaveContext& Context);

        void WriteImports(FPackageSaver& Ar, FPackageHeader& Header, FSaveContext& SaveContext);
        void WriteExports(FPackageSaver& Ar, FPackageHeader& Header, FSaveContext& SaveContext);
                
        /** Serializes the object's data; no-op if OF_NeedsLoad is unset. */
        RUNTIME_API void LoadObject(CObject* Object);
        RUNTIME_API CObject* LoadObject(const FGuid& GUID);
        RUNTIME_API CObject* LoadObjectByName(const FName& Name);

        RUNTIME_API NODISCARD bool FullyLoad();

        RUNTIME_API CObject* FindObjectInPackage(const FName& Name);
        
        RUNTIME_API NODISCARD CObject* IndexToObject(const FObjectPackageIndex& Index);

#if USING(WITH_EDITOR)
        /** Editor-only thumbnail data; not present in non-editor builds. */
        RUNTIME_API NODISCARD FPackageThumbnail* GetPackageThumbnail();
#endif

        RUNTIME_API NODISCARD FFixedString GetPackagePath() const;
        
        RUNTIME_API void MarkDirty() { if (!IsTransientPackage()) bDirty = true; }
        RUNTIME_API void ClearDirty() { bDirty = false; }
        RUNTIME_API NODISCARD bool IsDirty() const { return bDirty; }
        
        template<typename T>
        static void AddPackageExt(T& String)
        {
            String += ".lasset";
        }
        
    public:
        
        RUNTIME_API static FPackageDestroyedDelegate OnPackageDestroyed;

        uint32                           bDirty:1 = false;
        
        TSharedPtr<FPackageFileBytes>    LoaderBytes;
        TVector<FObjectImport>           ImportTable;
        TVector<FObjectExport>           ExportTable;

    private:

        // Phased-load steps for LoadAssetGraph, each one task over this package's whole ExportTable.

        // Creates every export's shell with OF_NeedsLoad, so a cross-package reference resolves before any read.
        void CreateExportShells();

        // Serializes every export, deferring PostLoad so references resolve to shells without a nested load.
        void SerializeExports();

        // PostLoad every export still owing one (OF_NeedsPostLoad). Called leaf-first across packages.
        void PostLoadExports();

        // Create/find one export's shell (no data load). Returns null only if its class can't be resolved.
        CObject* CreateExportShell(int32 ExportIndex);

        // Reads one export's data and marks OF_NeedsPostLoad, which is what splits serialize from PostLoad.
        void SerializeObject(CObject* Object);

        // Run a deferred PostLoad if one is owed; idempotent (clears OF_NeedsPostLoad first).
        static void PostLoadObject(CObject* Object);

        // Re-opens the on-disk bytes when the cache was dropped, false for a package with no backing file.
        bool EnsureLoader();

        // Drops this package's reference to the cached bytes once every export is resident, never a reader's.
        void ConditionalDropLoader();

        /** False for a dirty package, whose payloads may no longer match the bytes a splice would carry across. */
        bool CanSpliceBulkRegion() const;

        /** Publishes the region and the file it lives in together, so a streaming worker never sees half a save. */
        void SetBulkSource(const FBulkRegion& Region, FStringView Path);

        TAtomic<ELoadState>             LoadState{ELoadState::Unloaded};

        // Located at load from the file's trailer and refreshed on save, zero when no export wrote anything bulk.
        FBulkRegion                     BulkRegion;

        // The file BulkRegion indexes into, since a rename saves under a name that does not hold those bytes yet.
        FFixedString                    BulkSourcePath;

        // Guards the region and path as a pair, held only to copy them, never across the disk read itself.
        mutable FMutex                  BulkMutex;

        // Bumped whenever the pair is republished, which is what dates a ref taken against an older save.
        uint32                          BulkGeneration = 1;

        // Set when an export could not re-read a payload, which refuses the save instead of emptying it.
        bool                            bBulkDataUnresolved = false;

        // Set while a save splices the existing bulk region, which exports read through IsBulkPassthrough.
        bool                            bBulkPassthrough = false;

        // Version every reader over LoaderBytes is stamped with, or an older asset misparses on reload.
        int32                           LoaderFileVersion = 0;

        // Guards publication of LoaderBytes only. Reads run outside it, one cursor each.
        mutable FMutex                  LoaderBytesMutex;
#if USING(WITH_EDITOR)
        mutable FMutex                  ThumbnailMutex;
        TUniquePtr<FPackageThumbnail>   PackageThumbnail;
#endif
    };
    
}
