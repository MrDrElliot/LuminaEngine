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
                Texture->ConditionalBeginDestroy();
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
        // CPU mips only; see the mesh importer. The asset is saved and released here, and gets its GPU
        // image from CTexture::PostLoad the first time something loads it.
        CookRequest.bCreateGPUResource = false;

        CTexture* Texture = CFactory::CreateNewOf<CTexture>(PackagePath);
        if (Texture == nullptr)
        {
            OutResult.Error = FString("A package already exists at ") + FString(PackagePath.c_str());
            return;
        }

        Texture->SetFlag(OF_NeedsPostLoad);
        // Set before the cook: the mip policy is applied while the CPU chain is built, not after.
        Texture->Group = Group;

        if (!CTextureFactory::CookIntoTexture(Texture, CookRequest))
        {
            Texture->ConditionalBeginDestroy();
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
        // Mesh-embedded textures qualify too: reimport supplies the file, so a texture that arrived with no
        // SourcePath of its own is exactly the case this exists to rescue.
        if (AssetClass == nullptr || !AssetClass->IsChildOf(CTexture::StaticClass()))
        {
            return false;
        }

        // Render targets are CTextures, but the renderer writes their contents; anything reimported into
        // one is gone on the next draw.
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

        // Recook reads SourcePath, so point it at the new file first. Restored on failure: a half-applied
        // reimport that claims a source it was never cooked from is worse than no change.
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
