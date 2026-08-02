#pragma once
#include "Core/Object/Object.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/Archiver.h"

namespace Lumina
{

    class CPackage;

    class FSaveContext
    {
    public:

        FSaveContext() = delete;
        FSaveContext(CPackage* InPackage)
            :CurrentPackage(InPackage)
        {}

        friend class FSaveReferenceBuilderArchive;

        /** Returns true if Export was newly inserted (caller should recurse into Serialize). */
        bool AddExport(CObject* Export);


        THashSet<CObject*> SeenExports;

        TVector<CObject*> Exports;

        CPackage* CurrentPackage;
    };

    /** This archiver will traverse an object hierarchy and find any references and build a save context. */
    class FSaveReferenceBuilderArchive : public FArchive
    {
    public:
        
        using FArchive::operator<<;

        FSaveReferenceBuilderArchive() = delete;
        FSaveReferenceBuilderArchive(FSaveContext* SaveContext)
            : SaveContext(SaveContext)
        {
            this->SetFlag(EArchiverFlags::Writing);
        }

        virtual FArchive& operator<<(CObject*& Value) override;
        virtual FArchive& operator<<(FObjectHandle& Value) override;

    private:

        FSaveContext* SaveContext;
    };
    
    struct FObjectImport;

    class FPackageSaver : public FMemoryWriter
    {
    public:

        using FArchive::operator<<;

        explicit FPackageSaver(TVector<uint8>& InBytes, CPackage* InPackage)
            : FMemoryWriter(InBytes, false)
            , Package(InPackage)
        {}

        virtual FArchive& operator<<(CObject*& Value) override;
        virtual FArchive& operator<<(FObjectHandle& Value) override;

        /** Folds the soft target's GUID into the ImportTable as a Soft edge;
         *  a GUID already present as a hard import keeps the hard slot. */
        virtual void RegisterSoftAssetReference(const FGuid& AssetGUID) override;

        /** Build the ImportTable: hard imports first (discovery order),
         *  then soft imports not already present as hard. */
        void PopulateImportTable(TVector<FObjectImport>& Out) const;

        uint32 GetImportCount() const { return CurrentImportIndex; }

    private:

        CPackage*                   Package;
        THashMap<CObject*, uint32>  ObjectToIndexMap;
        THashSet<FGuid>             SoftReferencedGUIDs;
        uint32                      CurrentImportIndex = 0;
    };
}
