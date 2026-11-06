#pragma once

#include "Containers/Function.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/Object.h"
#include "Core/Object/Cast.h"
#include "Memory/SmartPtr.h"
#include "Core/Object/ObjectHandleTyped.h"
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

        // The factory for Class itself, else one for a base class it derives from.
        CFactory* FindFactory(CClass* Class) const;

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

        // Creates, saves and registers one Class asset, and takes it down again when the save fails so its name stays free.
        static TStrongObjectPtr<CObject> CreateAsset(CFactory* Factory, CClass* Class, FStringView Path, FString& OutError);

        // Takes down an asset created here that nothing else has seen, with its package, so the name is free again.
        static void DiscardNew(CObject* Asset);
        static void DiscardNewPackage(CPackage* Package);
            
        
        virtual FString GetAssetName() const { return ""; }
        virtual FString GetAssetDescription() const { return ""; }
        virtual CClass* GetAssetClass() const { return nullptr; }

        // Groups this asset type in the content browser's "New Asset" menu. Keep the set of
        // category strings small and shared across related factories.
        virtual FString GetCategory() const { return "Miscellaneous"; }
        virtual FStringView GetDefaultAssetCreationName() { return "New_Asset"; }
        
        virtual CObject* CreateNew(const FName& Name, CPackage* Package) { return nullptr; }

        // Importing source files is CImporter's job (Tools/Import/Importer.h); a factory only mints empty
        // assets from the content browser's New Asset menu.

        // True once the dialogue is done, with bOutCreating saying whether it handed creation to a task rather than canceled.
        static bool ShowCreationDialogue(CFactory* Factory, FStringView Path, bool& bOutCreating);

        virtual bool HasCreationDialogue() const { return false; }

    protected:
        
        virtual bool DrawCreationDialogue(FStringView Path, bool& bShouldClose) { return true; }

        static void DrawCreationDialogueTitle(const char* Title);

        // True when Create is pressed, while Cancel sets bShouldClose.
        static bool DrawCreationDialogueButtons(bool bCanCreate, bool& bShouldClose);

    };
}
