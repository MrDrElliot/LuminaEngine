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
        // Nothing outside the import has seen this package, so a failed cook takes it down with the texture.
        static void DiscardFailedCook(CTexture* Texture)
        {
            CPackage* Package = Texture->GetPackage();

            Texture->ConditionalBeginDestroy();

            if (Package == nullptr || Package->IsTransientPackage() || Package->HasAnyFlag(OF_MarkedDestroy))
            {
                return;
            }

            Package->ClearDirty();
            Package->SetFlag(OF_MarkedDestroy);
            Package->RemoveFromRoot();
            Package->ConditionalBeginDestroy();
        }

        CTexture* ImportTextureAsset(const FFixedString& PackagePath, const FTextureCookRequest& Request)
        {
            CTexture* Texture = CFactory::CreateNewOf<CTexture>(PackagePath);
            if (Texture == nullptr)
            {
                LOG_ERROR("[TextureImport] could not create '{0}'; a package already exists at that path", PackagePath.c_str());
                return nullptr;
            }

            Texture->SetFlag(OF_NeedsPostLoad);

            if (!CTextureFactory::CookIntoTexture(Texture, Request))
            {
                DiscardFailedCook(Texture);
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

        CTexture* Texture = CFactory::CreateNewOf<CTexture>(PackagePath);
        if (Texture == nullptr)
        {
            OutResult.Error = FString("A package already exists at ") + FString(PackagePath.c_str());
            return;
        }

        Texture->SetFlag(OF_NeedsPostLoad);
        // Set before the cook, since the mip policy applies while the CPU chain is built.
        Texture->Group = Group;

        if (!CTextureFactory::CookIntoTexture(Texture, CookRequest))
        {
            Import::Textures::DiscardFailedCook(Texture);
            OutResult.Error = FString("Failed to cook ") + FString(Request.SourcePath.c_str());
            return;
        }

        if (Progress)
        {
            Progress->EnterProgressFrame(0.9f, "Saving package...");
        }

        CPackage* Package = Texture->GetPackage();
        if (CPackage::SavePackage(Package, Package->GetPackagePath()))
        {
            FAssetRegistry::Get().AssetCreated(Texture);
        }
        else
        {
            LOG_ERROR("[TextureImport] failed to save '{0}'; asset will not be registered", Package->GetPackagePath());
        }

        OutResult.CreatedObjects.push_back(Texture);
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
        Texture->SourcePath = FString(Request.SourcePath.c_str());

        if (!CTextureFactory::Recook(Texture))
        {
            Texture->SourcePath = PreviousSource;
            return false;
        }

        return true;
    }
}
