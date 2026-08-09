#pragma once

#include "Containers/Array.h"
#include "Containers/String.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectMacros.h"
#include "Importer.generated.h"

namespace Lumina
{
    class CStruct;
    class FScopedSlowTask;

    struct FImportRequest
    {
        FFixedString SourcePath;

        /** Destination package path, extension already stripped. */
        FFixedString DestinationPath;
    };

    struct FImportResult
    {
        /** Everything the import minted, in creation order; the caller saves and tears down in reverse. */
        TVector<CObject*> CreatedObjects;

        FString Error;

        NODISCARD bool Succeeded() const { return Error.empty(); }
    };

    /**
     * One source-format importer. The instance IS the import: its reflected properties are the settings the
     * editor edits through FPropertyTable, and its members hold whatever intermediate state the source parse
     * produced. The pipeline runs ParseSource (off-thread) -> settings -> BuildAssets (off-thread), then the
     * instance is discarded.
     */
    REFLECT()
    class EDITOR_API CImporter : public CObject
    {
        GENERATED_BODY()
    public:

        void PostCreateCDO() override;

        /** Each including the leading dot. */
        virtual void GetSupportedExtensions(TVector<FStringView>& OutExtensions) const { }

        virtual bool SupportsExtension(FStringView Ext) const;

        /** Higher wins when two importers claim the same extension. */
        virtual int32 GetPriority() const { return 0; }

        virtual FStringView GetImporterDisplayName() const { return "Asset"; }

        /** False imports immediately with defaults, no settings dialog. */
        virtual bool HasSettingsDialogue() const { return false; }

        /** Parse + discovery + dedup. Runs on a worker; must not touch the main thread. */
        virtual bool ParseSource(const FImportRequest& Request, FString& OutError, FScopedSlowTask* Progress) { return true; }

        /** Main-thread pass over the parse result, for work the worker deliberately skipped (thumbnails). */
        virtual void PrepareSettingsPreview() { }

        /** Processing + asset creation + serialization. Runs on a worker. */
        virtual void BuildAssets(const FImportRequest& Request, FImportResult& OutResult, FScopedSlowTask* Progress) { }

        /** Drawn under the reflected property table in the import dialogue. */
        virtual void DrawSourcePreview() { }

        /** Drops the parsed source data; called once the import is committed or cancelled. */
        virtual void ReleaseSourceData() { }

        //~ Reimport replaces the DATA of an existing asset: same CObject, same GUID, same package path, so
        //~ every reference to it survives.

        virtual bool CanReimport(const CStruct* AssetClass) const { return false; }
        virtual bool ReimportAsset(CObject* Asset, const FImportRequest& Request, FScopedSlowTask* Progress) { return false; }
        virtual FString GetReimportSourcePath(const CObject* Asset) const { return FString(); }
    };

    REFLECT()
    class EDITOR_API CImporterRegistry : public CObject
    {
        GENERATED_BODY()
    public:

        static CImporterRegistry& Get();

        void RegisterImporter(CImporter* Importer);

        const TVector<CImporter*>& GetImporters() const { return Importers; }

        /** The highest-priority importer CDO claiming Ext, or null. */
        CImporter* FindImporterForExtension(FStringView Ext) const;

        CImporter* FindReimporter(const CStruct* AssetClass) const;

        /**
         * A fresh instance for one import, seeded from the CDO's property defaults and rooted for the
         * duration. Null when nothing claims the extension. Release with DestroyImporter.
         */
        CImporter* CreateImporterFor(FStringView SourcePath) const;

        static CImporter* CreateImporterOfClass(CClass* ImporterClass);
        static void DestroyImporter(CImporter* Importer);

        NODISCARD bool IsExtensionSupported(FStringView Ext) const;

        /** Win32 double-null-terminated filter spanning every registered importer. */
        FFixedString BuildFileDialogFilter() const;

    private:

        TVector<CImporter*> Importers;
    };
}
