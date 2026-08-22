#include "Core/Threading/Thread.h"
#include "EditorPCH.h"
#include "Core/CoreEditorDelegates.h"
#include "Factory.h"

#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Core/Engine/Engine.h"
#include "Core/Object/Package/Package.h"
#include "FileSystem/FileSystem.h"
#include "Paths/Paths.h"
#include "TaskSystem/TaskSystem.h"
#include "Tools/Import/ImportHelpers.h"
#include "Log/Log.h"

namespace Lumina
{

    static CFactoryRegistry* FactoryRegistry = nullptr;
    
    CFactoryRegistry& CFactoryRegistry::Get()
    {
        static FOnceFlag Flag;
        CallOnce(Flag, []()
        {
            FactoryRegistry = NewObject<CFactoryRegistry>();
            FactoryRegistry->AddToRoot();
        });

        return *FactoryRegistry;
    }

    void CFactoryRegistry::RegistryFactory(CFactory* Factory)
    {
        Factories.push_back(Factory);
    }

    void CFactory::PostCreateCDO()
    {
        CFactoryRegistry::Get().RegistryFactory(this);
    }
    
    CObject* CFactory::TryCreateNew(FStringView Path)
    {
        FFixedString SafePath = SanitizeObjectName(Path);
        CPackage* Package = CPackage::CreatePackage(SafePath);
        if (Package == nullptr)
        {
            return nullptr;
        }
        
        FStringView FileName = VFS::FileName(Path, true);

        CObject* New = CreateNew(FileName, Package);
        if (New == nullptr)
        {
            return nullptr;
        }
        
        Package->ExportTable.emplace_back(New);
        
        New->SetFlag(OF_Public);

        return New;
    }

    CObject* CFactory::CreateNewOf(CClass* Class, FStringView Path)
    {
        FFixedString SafePath = SanitizeObjectName(Path);
        CPackage* Package = CPackage::CreatePackage(SafePath);
        if (Package == nullptr)
        {
            return nullptr;
        }

        FStringView FileName = VFS::FileName(Path, true);

        CObject* New = NewObject(Class, Package, FileName, FGuid::New(), OF_Public);
        Package->ExportTable.emplace_back(New);
        
        return New;
    }

    bool CFactory::ShowCreationDialogue(CFactory* Factory, FStringView Path)
    {
        bool bShouldClose = false;
        if (Factory->DrawCreationDialogue(Path, bShouldClose))
        {
            // Modal capture dies when the modal closes; async task needs an owned copy.
            FFixedString OwnedPath(Path.data(), Path.size());
            Task::AsyncTask(1, 1, [Factory, OwnedPath = Move(OwnedPath)](uint32, uint32, uint32)
            {
                CObject* NewAsset = Factory->TryCreateNew(OwnedPath);
                if (NewAsset == nullptr)
                {
                    return;
                }

                CPackage* Package = NewAsset->GetPackage();
                if (CPackage::SavePackage(Package, OwnedPath))
                {
                    FAssetRegistry::Get().AssetCreated(NewAsset);
                    FCoreEditorDelegates::OnAssetCreated.Broadcast(NewAsset);
                }
                else
                {
                    LOG_ERROR("Factory: failed to save {}; asset will not be registered", OwnedPath);
                }
            });

            return true;
        }

        return bShouldClose;
    }
}
