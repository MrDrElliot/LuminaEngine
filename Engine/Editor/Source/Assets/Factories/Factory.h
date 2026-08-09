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

        // Importing source files is CImporter's job (Tools/Import/Importer.h); a factory only mints empty
        // assets from the content browser's New Asset menu.

        static bool ShowCreationDialogue(CFactory* Factory, FStringView Path);

        virtual bool HasCreationDialogue() const { return false; }

    protected:
        
        virtual bool DrawCreationDialogue(FStringView Path, bool& bShouldClose) { return true; }
        
    };
}
