#include "Core/Threading/Thread.h"
#include "EditorPCH.h"
#include "Factory.h"

#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Core/Engine/Engine.h"
#include "Core/Object/Package/Package.h"
#include "FileSystem/FileSystem.h"
#include "Paths/Paths.h"
#include "TaskSystem/TaskSystem.h"
#include "TaskSystem/ThreadedCallback.h"
#include "Tools/Import/ImportHelpers.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
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
        Factories.Add(Factory);
    }

    CFactory* CFactoryRegistry::FindFactory(CClass* Class) const
    {
        CFactory* Found = nullptr;
        for (CFactory* Factory : Factories)
        {
            CClass* AssetClass = Factory != nullptr ? Factory->GetAssetClass() : nullptr;
            if (AssetClass != nullptr && Class->IsChildOf(AssetClass) && (Found == nullptr || AssetClass == Class))
            {
                Found = Factory;
            }
        }
        return Found;
    }

    void CFactory::PostCreateCDO()
    {
        CFactoryRegistry::Get().RegistryFactory(this);
    }
    
    namespace
    {
        // Every new asset starts as the one public export of a fresh package, which is discarded again when nothing comes of it.
        template<typename TMake>
        CObject* CreateInNewPackage(FStringView Path, TMake&& Make)
        {
            CPackage* Package = CPackage::CreatePackage(Path);
            if (Package == nullptr)
            {
                return nullptr;
            }

            CObject* New = Make(FName(VFS::FileName(Path, true)), Package);
            if (New == nullptr)
            {
                CFactory::DiscardNewPackage(Package);
                return nullptr;
            }

            Package->ExportTable.Emplace(New);
            New->SetFlag(OF_Public);
            return New;
        }
    }

    CObject* CFactory::TryCreateNew(FStringView Path)
    {
        return CreateInNewPackage(Path, [this](const FName& Name, CPackage* Package) { return CreateNew(Name, Package); });
    }

    CObject* CFactory::CreateNewOf(CClass* Class, FStringView Path)
    {
        return CreateInNewPackage(Path, [Class](const FName& Name, CPackage* Package) { return NewObject(Class, Package, Name, FGuid::New(), OF_Public); });
    }

    TStrongObjectPtr<CObject> CFactory::CreateAsset(CFactory* Factory, CClass* Class, FStringView Path, FString& OutError)
    {
        // Held through the save, since a fresh object nothing holds dies at the first transient strong reference.
        TStrongObjectPtr<CObject> Asset(Factory->GetAssetClass() == Class ? Factory->TryCreateNew(Path) : CreateNewOf(Class, Path));
        if (!Asset)
        {
            OutError = "it could not be created";
            return {};
        }

        CPackage* Package = Asset->GetPackage();
        if (!CPackage::SavePackage(Package, Path))
        {
            OutError = "it could not be saved";
            Asset.Reset();
            DiscardNewPackage(Package);
            return {};
        }

        FAssetRegistry::Get().AssetCreated(Asset.Get());
        return Asset;
    }

    void CFactory::DiscardNew(CObject* Asset)
    {
        CPackage* Package = Asset->GetPackage();
        Asset->ConditionalBeginDestroy();
        DiscardNewPackage(Package);
    }

    void CFactory::DiscardNewPackage(CPackage* Package)
    {
        if (Package == nullptr || Package->IsTransientPackage() || Package->HasAnyFlag(OF_MarkedDestroy))
        {
            return;
        }

        Package->ClearDirty();
        Package->SetFlag(OF_MarkedDestroy);
        // The root set holds the only strong reference, so unrooting is what destroys it.
        Package->RemoveFromRoot();
    }

    bool CFactory::ShowCreationDialogue(CFactory* Factory, FStringView Path, bool& bOutCreating)
    {
        bOutCreating = false;
        bool bShouldClose = false;
        if (Factory->DrawCreationDialogue(Path, bShouldClose))
        {
            // Modal capture dies when the modal closes; async task needs an owned copy.
            FFixedString OwnedPath(Path.GetData(), Path.Len());
            Jobs::Spawn(Jobs::EJobPriority::Normal, [](CFactory* Owner, FFixedString TargetPath) -> Jobs::TTask<void>
            {
                FString Error;
                const bool bCreated = CreateAsset(Owner, Owner->GetAssetClass(), TargetPath, Error).IsValid();

                // Reported only once the task knows, since the dialog closed before the asset existed.
                MainThread::Enqueue([bCreated, TargetPath, Error = Move(Error)]
                {
                    if (bCreated)
                    {
                        ImGuiX::Notifications::NotifySuccess("Successfully Created: \"{0}\"", TargetPath);
                    }
                    else
                    {
                        ImGuiX::Notifications::NotifyError("Failed to create \"{0}\": {1}", TargetPath, Error);
                    }
                });
                co_return;
            }, Factory, Move(OwnedPath));

            bOutCreating = true;
            return true;
        }

        return bShouldClose;
    }

    void CFactory::DrawCreationDialogueTitle(const char* Title)
    {
        ImGui::TextColored(EditorColors::SectionHeader(), "%s", Title);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    bool CFactory::DrawCreationDialogueButtons(bool bCanCreate, bool& bShouldClose)
    {
        ImGui::Spacing();
        ImGui::Separator();

        const float ButtonWidth = 110.0f;
        const float Spacing = ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ButtonWidth * 2 - Spacing);

        ImGui::BeginDisabled(!bCanCreate);
        const bool bConfirm = ImGui::Button(LE_ICON_CHECK " Create", ImVec2(ButtonWidth, 0));
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_CLOSE " Cancel", ImVec2(ButtonWidth, 0)))
        {
            bShouldClose = true;
        }

        return bConfirm;
    }
}
