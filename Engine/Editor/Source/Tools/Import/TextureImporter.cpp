#include "EditorPCH.h"
#include "TextureImporter.h"

#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Textures/TextureRenderTarget.h"
#include "Assets/Factories/Factory.h"
#include "Assets/Factories/TextureFactory/TextureFactory.h"
#include "Core/Object/Package/Package.h"
#include "Core/Progress/SlowTask.h"
#include "Log/Log.h"

namespace Lumina
{
    namespace Import::Textures
    {
        CTexture* ImportTextureAsset(const FFixedString& PackagePath, const FTextureCookRequest& Request, ETextureGroup Group)
        {
            CTexture* Texture = CFactory::CreateNewOf<CTexture>(PackagePath);
            if (Texture == nullptr)
            {
                LOG_ERROR("[TextureImport] could not create '{0}'; a package already exists at that path", *PackagePath);
                return nullptr;
            }

            Texture->SetFlag(OF_NeedsPostLoad);
            // Set before the cook, since the mip policy applies while the CPU chain is built.
            Texture->Group = Group;

            if (!CTextureFactory::CookIntoTexture(Texture, Request))
            {
                CFactory::DiscardNew(Texture);
                return nullptr;
            }

            return Texture;
        }
    }

    void CTextureImporter::BuildAssets(const FImportRequest& Request, FImportResult& OutResult, FScopedSlowTask* Progress)
    {
        if (Progress)
        {
            Progress->EnterProgressFrame(0.1f, "Cooking texture...");
        }

        FFixedString PackagePath = Request.DestinationPath;
        CPackage::AddPackageExt(PackagePath);

        Import::Textures::FTextureCookRequest CookRequest;
        CookRequest.SourcePath = Request.SourcePath;
        CookRequest.ColorSpace = ColorSpace;
        // The asset gets its GPU image from CTexture::PostLoad the first time something loads it.
        CookRequest.bCreateGPUResource = false;

        if (CPackage::FindPackageByPath(PackagePath) != nullptr)
        {
            OutResult.Error = FString("A package already exists at ") + FString(*PackagePath);
            return;
        }

        CTexture* Texture = Import::Textures::ImportTextureAsset(PackagePath, CookRequest, Group);
        if (Texture == nullptr)
        {
            OutResult.Error = FString("Failed to cook ") + FString(*Request.SourcePath);
            return;
        }

        if (Progress)
        {
            Progress->EnterProgressFrame(0.9f, "Saving package...");
        }

        CPackage* Package = Texture->GetPackage();
        if (!CPackage::SavePackage(Package, Package->GetPackagePath()))
        {
            // Discarded rather than left unsaved in memory, where it would make every retry at this path skip.
            OutResult.Error = FString("Failed to save ") + FString(*Package->GetPackagePath());
            CFactory::DiscardNew(Texture);
            return;
        }

        FAssetRegistry::Get().AssetCreated(Texture);
        OutResult.CreatedObjects.Add(Texture);
    }

    bool CTextureImporter::CanReimport(const CStruct* AssetClass) const
    {
        // A mesh-embedded texture has no SourcePath of its own, which is exactly what this rescues.
        if (AssetClass == nullptr || !AssetClass->IsChildOf(CTexture::StaticClass()))
        {
            return false;
        }

        // The renderer writes a render target's contents, so anything reimported into one is gone next draw.
        return !AssetClass->IsChildOf(CTextureRenderTarget::StaticClass());
    }

    FString CTextureImporter::GetReimportSourcePath(const CObject* Asset) const
    {
        const CTexture* Texture = Cast<CTexture>(Asset);
        return Texture != nullptr ? Texture->SourcePath : FString();
    }

    bool CTextureImporter::ReimportAsset(CObject* Asset, const FImportRequest& Request, FScopedSlowTask* Progress)
    {
        CTexture* Texture = Cast<CTexture>(Asset);
        if (Texture == nullptr)
        {
            return false;
        }

        // Restored on failure, since claiming a source it was never cooked from is worse than no change.
        const FString PreviousSource = Texture->SourcePath;
        Texture->SourcePath = FString(*Request.SourcePath);

        if (!CTextureFactory::Recook(Texture))
        {
            Texture->SourcePath = PreviousSource;
            return false;
        }

        return true;
    }
}
