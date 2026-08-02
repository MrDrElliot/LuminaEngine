#pragma once

#include "Lumina.h"
#include "Assets/AssetRegistry/AssetData.h"
#include "Core/Delegates/Delegate.h"
#include "Core/Object/Class.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/Package/Thumbnail/PackageThumbnail.h"
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

        /** Cook-graph edge classification. Hard for direct CObject* refs;
         *  Soft for FSoftObjectPath via FArchive::RegisterSoftAssetReference. */
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
    
    /** Negative = import, positive = export, 0 = null. Encoded as Import: -(i+1), Export: i+1. */
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
        
        enum class ELoadState
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

        /** Atomic rename: in-memory + on-disk move. Crash-safe (write-then-remove). False on collision/IO error. */
        RUNTIME_API NODISCARD static bool RenamePackage(FStringView OldPath, FStringView NewPath);

        /** Update in-memory identity when a parent dir was renamed externally; file name unchanged, no disk I/O. */
        RUNTIME_API static void OnPackageMovedExternally(FStringView OldPath, FStringView NewPath);


        /** Idempotent. Returns shells marked OF_NeedsLoad; objects are not yet serialized. */
        RUNTIME_API static CPackage* LoadPackage(FStringView Path);

        /** Fully loads RootGUID and its entire Hard/Owned dependency closure with a parallel, phased loader:
         *  discover the package set from the asset registry, read+inflate every package in parallel, create
         *  all object shells, serialize all data in parallel (refs resolve to shells), then PostLoad
         *  leaf-first. Falls back to the plain inline load if RootGUID isn't a registered asset. Returns the
         *  loaded root object. Use this for top-level asset opens (worlds, large assets) that fan out widely. */
        RUNTIME_API static CObject* LoadAssetGraph(const FGuid& RootGUID);

        RUNTIME_API static bool SavePackage(CPackage* Package, FStringView Path);

        /** Save a loaded package to compressed bytes for the cooker (Cooking flag set);
         *  strips EditorOnly properties and thumbnails. */
        RUNTIME_API NODISCARD static bool SavePackageForCook(CPackage* Package, TVector<uint8>& OutCompressed);

        /** Reads a package file from disk and decompresses it into the raw uncompressed package binary. */
        RUNTIME_API static bool ReadPackageFile(FStringView Path, TVector<uint8>& OutBinary);

        void CreateLoader(const TVector<uint8>& FileBinary);
        
        RUNTIME_API FPackageLoader* GetLoader() const;

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
        
        RUNTIME_API void MarkDirty() { bDirty = true; }
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
        
        TUniquePtr<FArchive>             Loader;
        TVector<FObjectImport>           ImportTable;
        TVector<FObjectExport>           ExportTable;

    private:

        // Phased-load building blocks used by LoadAssetGraph. Each runs over this package's whole ExportTable
        // on a single task (cross-package parallelism comes from running these per-package concurrently).

        // Create (or find) every export's object shell with OF_NeedsLoad set, so cross-package references can
        // resolve to a live pointer before any data is serialized.
        void CreateExportShells();

        // Serialize every export's data. During a graph load PostLoad is deferred (objects come out marked
        // OF_NeedsPostLoad); references resolve to already-created shells without triggering a nested load.
        void SerializeExports();

        // PostLoad every export still owing one (OF_NeedsPostLoad). Called leaf-first across packages.
        void PostLoadExports();

        // Create/find one export's shell (no data load). Returns null only if its class can't be resolved.
        CObject* CreateExportShell(int32 ExportIndex);

        // Read one export's data from the loader (PreLoad + Serialize). Marks OF_NeedsPostLoad instead of
        // PostLoading, so serialize and PostLoad can be separated into phases.
        void SerializeObject(CObject* Object);

        // Run a deferred PostLoad if one is owed; idempotent (clears OF_NeedsPostLoad first).
        static void PostLoadObject(CObject* Object);

        // Re-open the on-disk loader if its cached bytes were dropped after a full load. Returns false when
        // no backing file is available (transient / never-saved package), leaving Loader null.
        bool EnsureLoader();

        // Free the cached uncompressed file bytes once every export is resident. A still-unloaded export
        // keeps them (re-read lazily via EnsureLoader). No-op while a load is in flight on this package.
        void ConditionalDropLoader();

        TAtomic<ELoadState>             LoadState{ELoadState::Unloaded};

        // Reentrancy depth of object loads sharing this package's Loader. The cached bytes are dropped only
        // when the outermost load unwinds, so a nested IndexToObject load can't free the buffer mid-read.
        int32                           ActiveLoadDepth = 0;
#if USING(WITH_EDITOR)
        mutable FMutex                  ThumbnailMutex;
        TUniquePtr<FPackageThumbnail>   PackageThumbnail;
#endif
    };
    
}
