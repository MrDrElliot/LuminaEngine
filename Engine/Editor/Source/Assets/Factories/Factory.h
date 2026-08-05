#pragma once

#include "Containers/Function.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/Object.h"
#include "Core/Object/Cast.h"
#include "Memory/SmartPtr.h"
#include "Factory.generated.h"

namespace Lumina
{
    namespace Import
    {
        struct FImportSettings;
    }

    class CFactory;
    
    REFLECT()
    class EDITOR_API CFactoryRegistry : public CObject
    {
        GENERATED_BODY()
    public:

        static CFactoryRegistry& Get();
        
        void RegistryFactory(CFactory* Factory);

        const TVector<CFactory*>& GetFactories() const { return Factories; }

        TVector<CFactory*> Factories;
    };
    
    REFLECT()
    class EDITOR_API CFactory : public CObject
    {
        GENERATED_BODY()

    public:

        void PostCreateCDO() override;

        template<Concept::IsACObject T>
        T* TryCreateNew(FStringView Path)
        {
            return Cast<T>(TryCreateNew(Path));
        }
        
        CObject* TryCreateNew(FStringView Path);
        static CObject* CreateNewOf(CClass* Class, FStringView Path);
        
        template<Concept::IsACObject T>
        static T* CreateNewOf(FStringView Path)
        {
            return Cast<T>(CreateNewOf(T::StaticClass(), Path));
        }
            
        
        virtual FString GetAssetName() const { return ""; }
        virtual FString GetAssetDescription() const { return ""; }
        virtual CClass* GetAssetClass() const { return nullptr; }

        // Groups this asset type in the content browser's "New Asset" menu. Keep the set of
        // category strings small and shared across related factories.
        virtual FString GetCategory() const { return "Miscellaneous"; }
        virtual FStringView GetDefaultAssetCreationName() { return "New_Asset"; }
        
        virtual CObject* CreateNew(const FName& Name, CPackage* Package) { return nullptr; }

        void Import(const FFixedString& ImportFile, const FFixedString& DestinationPath, const Import::FImportSettings* Settings);

        virtual bool CanImport() { return false; }
        virtual void TryImport(const FFixedString& ImportFilePath, const FFixedString& DestinationPath, const Import::FImportSettings* Settings) { }

        virtual bool IsExtensionSupported(FStringView Ext) { return false; }

        // Reimport REPLACES THE DATA of an asset that already exists: same CObject, same GUID, same package
        // path, so every reference to it survives. That is the whole point of it being separate from
        // TryImport, which mints new assets and side-steps name collisions by appending "_1".
        //
        // Asked by CLASS, not by instance, so a context menu can decide whether to offer the item without
        // loading the asset behind every tile it draws.
        virtual bool CanReimport(const CStruct* AssetClass) const { return false; }

        // Swaps SourceFile's contents onto Asset. Settings comes from the same PrepareImportAsync +
        // DrawImportSettings pass a fresh import uses, so the importer's options apply here too.
        // Returns false and leaves the asset untouched when the source cannot supply matching data.
        virtual bool TryReimport(CObject* Asset, const FFixedString& SourceFile, const Import::FImportSettings* Settings) { return false; }

        // File the asset was last imported from, so the reimport dialog opens there. Empty when unknown --
        // an asset imported before source paths were recorded, or one with no source file at all.
        virtual FString GetReimportSourcePath(const CObject* Asset) const { return FString(); }

        // The factory that can reimport this asset class, or null when none can.
        static CFactory* FindReimportFactory(const CStruct* AssetClass);

        // "Meshes (*.fbx;*.gltf)\0*.fbx;*.gltf\0All Files (*.*)\0*.*\0" style filter built from whatever
        // this factory's IsExtensionSupported accepts, for the OS file dialog.
        FFixedString BuildFileDialogFilter() const;

        // Extensions this factory accepts, each including the leading dot. Override alongside
        // IsExtensionSupported; the base pair keeps the dialog filter and the accept test from drifting.
        virtual void GetSupportedExtensions(TVector<FStringView>& OutExtensions) const { }
        
        static bool ShowCreationDialogue(CFactory* Factory, FStringView Path);

        virtual bool HasImportDialogue() const { return false; }
        virtual bool HasCreationDialogue() const { return false; }

        // Builds import settings for DrawImportDialogue, possibly off-thread. OnReady
        // runs on the main thread (null on failure). Default is synchronous.
        using FImportPrepareCallback = TMoveOnlyFunction<void(TUniquePtr<Import::FImportSettings>)>;
        virtual void PrepareImportAsync(const FFixedString& RawPath, const FFixedString& DestinationPath, FImportPrepareCallback OnReady);

        // Draws THIS FACTORY'S SETTINGS ONLY. The window around them -- source header, scroll region,
        // confirm and cancel -- belongs to the import window, so every importer gets the same one and a
        // batch can drive the buttons without reaching into a factory.
        virtual void DrawImportSettings(const FFixedString& RawPath, Import::FImportSettings& Settings) {}

        // The user confirmed: fold whatever the settings UI was holding into the settings object.
        virtual void CommitImportSettings(Import::FImportSettings& Settings) {}
        
    protected:
        
        virtual bool DrawCreationDialogue(FStringView Path, bool& bShouldClose) { return true; }
        
    };
}
