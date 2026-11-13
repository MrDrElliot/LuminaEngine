#pragma once

#include "Lumina.h"
#include "Assets/AssetRegistry/AssetData.h"
#include "Core/Delegates/Delegate.h"
#include "Core/Object/Class.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Threading/Sync.h"
#include "Core/Versioning/CoreVersion.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    struct FPackageThumbnail;
    struct FPackageFileBytes;
    struct FLoadRequest;
    class FSaveContext;
    class FPackageLoader;
    class FPackageSaver;
    class FPackageNameTable;
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
        FName ClassPackage;
        int64 Offset;
        int64 Size;
        TWeakObjectPtr<CObject> Object;

        FORCEINLINE friend FArchive& operator << (FArchive& Ar, FObjectExport& Data)
        {
            Ar << Data.ObjectGUID;
            Ar << Data.ObjectName;
            Ar << Data.ClassName;
            if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::EXPORT_CLASS_PACKAGE)
            {
                Ar << Data.ClassPackage;
            }
            Ar << Data.Offset;
            Ar << Data.Size;

            return Ar;
        }
    };

    // A type package of a module the shipped game never loads, whose objects in an asset are editor state such as a node graph.
    RUNTIME_API bool IsEditorTypePackage(const FName& PackageName);

    struct FObjectImport
    {
        FObjectImport() = default;
        FObjectImport(CObject* InObject);
        FObjectImport(const FGuid& InGUID, EDependencyType InType)
            : ObjectGUID(InGUID), Type(InType) {}

        FGuid ObjectGUID;
        // Resolved after the import is loaded.
        TWeakObjectPtr<CObject> Object;

        // Cook-graph edge classification, Soft for an FSoftObjectPath and Hard for a direct pointer.
        EDependencyType Type = EDependencyType::Hard;

        // The bundles whose properties reference this import, so the registry can load a bundle without its owner.
        TVector<FName> Bundles;

        FORCEINLINE friend FArchive& operator << (FArchive& Ar, FObjectImport& Data)
        {
            Ar << Data.ObjectGUID;
            uint8 T = static_cast<uint8>(Data.Type);
            Ar << T;
            if (Ar.IsReading())
            {
                Data.Type = static_cast<EDependencyType>(T);
            }
            if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::ASSET_BUNDLES)
            {
                Ar << Data.Bundles;
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
        // written last, since names are only known once the rest of the package has been serialized
        int64 NameTableOffset = 0;

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

            // branches on the version just read, so this works before the archive carries it
            if (Data.Version >= (int32)ELuminaEngineVersion::PACKAGE_NAME_TABLE)
            {
                Ar << Data.NameTableOffset;
            }

            return Ar;
        }
    };
    static_assert(std::is_standard_layout_v<FPackageHeader>, "FPackageHeader must only contain trivial data members");
    static_assert(std::is_trivially_copyable_v<FPackageHeader>, "FPackageHeader must only contain trivial data members");
    
    // Negative is an import, positive an export, 0 null, encoded as -(i+1) and i+1.
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
                return Constants::kIndexNone;
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

    // test only, pins the version SavePackage stamps and encodes at, 0 restores the current version
    RUNTIME_API void SetForcedPackageSaveVersion(int32 Version);

    // test only, pins the container's codec and compression level, a negative codec restores the default
    RUNTIME_API void SetForcedPackageCodec(int32 Codec, int32 Level);

    class CPackage : public CObject
    {
    public:

        DECLARE_CLASS(Lumina, CPackage, CObject, "", RUNTIME_API)
        DEFINE_CLASS_FACTORY(CPackage)
        
        enum class ELoadState : uint8
        {
            Unloaded,
            Loading,
            Loaded,
            Failed
        };

        CPackage();
        ~CPackage() override;
        void OnDestroy() override;
        bool Rename(const FName& NewName, CPackage* NewPackage) override;
        
        RUNTIME_API static CPackage* CreatePackage(FStringView Path);

        // Engine-wide in-memory package for runtime-only objects such as engine primitives, never saved.
        RUNTIME_API static CPackage* GetTransientPackage();

        RUNTIME_API bool IsTransientPackage() const;

        RUNTIME_API static bool DestroyPackage(FStringView Path);

        RUNTIME_API static bool DestroyPackage(CPackage* PackageToDestroy);

        RUNTIME_API static CPackage* FindPackageByPath(FStringView Path);

        // Atomic in-memory and on-disk rename, crash-safe by write-then-remove, false on collision or IO error.
        NODISCARD RUNTIME_API static bool RenamePackage(FStringView OldPath, FStringView NewPath);

        // Updates in-memory identity after a parent folder was renamed externally, without touching disk.
        RUNTIME_API static void OnPackageMovedExternally(FStringView OldPath, FStringView NewPath);


        // Idempotent, returning shells marked OF_NeedsLoad whose objects are not yet serialized.
        RUNTIME_API static CPackage* LoadPackage(FStringView Path);

        // Parallel phased load of RootGUID and its hard closure, falling back to the inline load when unregistered.
        RUNTIME_API static CObject* LoadAssetGraph(const FGuid& RootGUID);

        RUNTIME_API static bool SavePackage(CPackage* Package, FStringView Path);

        // Saves to compressed bytes for the cooker, stripping EditorOnly properties and thumbnails.
        NODISCARD RUNTIME_API static bool SavePackageForCook(CPackage* Package, TVector<uint8>& OutCompressed,
                                                             const TFunction<bool(const CObject*)>& ExcludeExport = {});

        // Cooks a single-export package from its file bytes without loading it, and is false when only a load and save can (bulk data, older version, several exports).
        NODISCARD RUNTIME_API static bool StripPackageForCook(FStringView Path, TVector<uint8>& OutCompressed);

        // Where a package's bulk region sits in its file, file-absolute, with the refs inside exports relative to it.
        struct FBulkRegion
        {
            int64 FileOffset = 0;
            int64 Size       = 0;

            bool IsValid() const { return Size > 0; }
        };

        // Reads and decompresses the package container, leaving the bulk region on disk and merely locating it.
        RUNTIME_API static bool ReadPackageFile(FStringView Path, TVector<uint8>& OutBinary, FBulkRegion* OutBulkRegion = nullptr);

        // ReadPackageFile plus the header check, so a truncated or foreign file is refused before anyone walks its tables.
        NODISCARD RUNTIME_API static bool ReadPackageContainer(FStringView Path, TVector<uint8>& OutBinary, FPackageHeader& OutHeader, FBulkRegion* OutBulkRegion = nullptr);

        // One ranged read of a bulk payload, uncached so the caller owns the bytes, and safe from a worker thread.
        NODISCARD RUNTIME_API bool ReadBulkData(const FBulkDataRef& Ref, TVector<uint8>& OutBytes, uint32 ExpectedGeneration = 0) const;

        // Bumped by every save, so a caller holding refs it captured earlier can tell they went stale.
        NODISCARD RUNTIME_API uint32 GetBulkGeneration() const;

        NODISCARD RUNTIME_API const FBulkRegion& GetBulkRegion() const { return BulkRegion; }

        // Reports a payload that could not be read back, which makes SavePackage refuse rather than write it empty.
        RUNTIME_API void FlagUnresolvedBulkData() { bBulkDataUnresolved = true; }

        // True while a save copies this bulk region across verbatim, where a PreSave that reads payloads back costs the whole saving.
        NODISCARD RUNTIME_API bool IsBulkPassthrough() const { return bBulkPassthrough; }

        // Publishes a copy of FileBinary as the resident bytes and returns it, with no name table until a reader parses one.
        TSharedPtr<FPackageFileBytes> CreateLoader(const TVector<uint8>& FileBinary);

        // the bytes, the version to stamp a reader with and the table to resolve its slots, re-read from disk when the cache was dropped
        TSharedPtr<FPackageFileBytes> AcquireLoaderBytes(int32& OutFileVersion, TSharedPtr<const FPackageNameTable>& OutNames);

        // leaves Ar where it found it, and reports an empty table for a file whose names are inline text
        RUNTIME_API static bool ReadNameTable(FArchive& Ar, const FPackageHeader& Header, FPackageNameTable& OutNames);

        // Logs and refuses a header that is not a package, that a newer engine wrote, or whose table offsets fall outside the container.
        NODISCARD RUNTIME_API static bool IsReadableHeader(const FPackageHeader& Header, FStringView Path, int64 ContainerSize);

        RUNTIME_API void BuildSaveContext(FSaveContext& Context);

        void WriteImports(FPackageSaver& Ar, FPackageHeader& Header, FSaveContext& SaveContext);
        void WriteExports(FPackageSaver& Ar, FPackageHeader& Header, FSaveContext& SaveContext);
                
        // Serializes the object's data, and does nothing unless OF_NeedsLoad is set.
        RUNTIME_API void LoadObject(CObject* Object);

        // Runs Object's owed PostLoad, waiting out one already running elsewhere; true only when this call ran it.
        RUNTIME_API static bool ConditionalPostLoad(CObject* Object);
        RUNTIME_API CObject* LoadObject(const FGuid& GUID);
        RUNTIME_API CObject* LoadObjectByName(const FName& Name);

        NODISCARD RUNTIME_API bool FullyLoad();

        RUNTIME_API CObject* FindObjectInPackage(const FName& Name);
        
        NODISCARD RUNTIME_API CObject* IndexToObject(const FObjectPackageIndex& Index);

#if USING(WITH_EDITOR)
        NODISCARD RUNTIME_API FPackageThumbnail* GetPackageThumbnail();
#endif

        NODISCARD RUNTIME_API FFixedString GetPackagePath() const;
        
        RUNTIME_API void MarkDirty() { if (!IsTransientPackage()) { bDirty = true; ++EditGeneration; } }
        RUNTIME_API void ClearDirty() { bDirty = false; }
        NODISCARD RUNTIME_API bool IsDirty() const { return bDirty; }

        // Bumped by every MarkDirty, so an editor can tell an edit happened even while the package stays dirty.
        NODISCARD uint32 GetEditGeneration() const { return EditGeneration; }
        
        template<typename T>
        static void AddPackageExt(T& String)
        {
            String += ".lasset";
        }
        
    public:
        
        RUNTIME_API static FPackageDestroyedDelegate OnPackageDestroyed;

        uint32                           bDirty:1 = false;
        uint32                           EditGeneration = 0;
        
        TSharedPtr<FPackageFileBytes>    LoaderBytes;
        TVector<FObjectImport>           ImportTable;
        TVector<FObjectExport>           ExportTable;

    private:

        friend class FPackageLoader;

        // Phased-load steps for LoadAssetGraph, each one task over this package's whole ExportTable.

        // Creates every export's shell with OF_NeedsLoad, so a cross-package reference resolves before any read.
        void CreateExportShells();

        // Serializes every export under Request, which defers the closure's PostLoads to the graph's wave pass.
        void SerializeExports(FLoadRequest& Request);

        // PostLoad every export still owing one (OF_NeedsPostLoad). Called leaf-first across packages.
        void PostLoadExports();

        // Create/find one export's shell (no data load). Returns null only if its class can't be resolved.
        CObject* CreateExportShell(int32 ExportIndex);

        // Reads one export's data and marks OF_NeedsPostLoad, which is what splits serialize from PostLoad.
        void SerializeObject(CObject* Object, FLoadRequest& Request);

        void LoadObject(CObject* Object, FLoadRequest& Request);
        CObject* LoadObject(const FGuid& GUID, FLoadRequest& Request);
        CObject* IndexToObject(const FObjectPackageIndex& Index, FLoadRequest& Request);

        // StaticLoadObject that keeps the request, so an import loaded mid-graph defers like the rest.
        static CObject* LoadImport(const FGuid& GUID, FLoadRequest& Request);

        // Drops this package's reference to the cached bytes once every export is resident, never a reader's.
        void ConditionalDropLoader();

        // False for a dirty package, whose payloads may no longer match the bytes a splice would carry across.
        bool CanSpliceBulkRegion() const;

        // Publishes the region and the file it lives in together, so a streaming worker never sees half a save.
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

        // LoaderBytes' own table, parsed on first use, shared so a reader mid-flight keeps the one it started with
        TSharedPtr<FPackageNameTable>   LoaderNames;

        // Guards publication of LoaderBytes only. Reads run outside it, one cursor each.
        mutable FMutex                  LoaderBytesMutex;

        // Two loads reaching one export at once would otherwise both create its shell; recursive for a shell whose construction loads a sibling.
        FRecursiveMutex                 ExportShellMutex;
#if USING(WITH_EDITOR)
        mutable FMutex                  ThumbnailMutex;
        TUniquePtr<FPackageThumbnail>   PackageThumbnail;
#endif
    };
    
}
