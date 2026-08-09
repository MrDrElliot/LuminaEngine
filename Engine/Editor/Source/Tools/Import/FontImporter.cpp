#include "EditorPCH.h"
#include "FontImporter.h"

#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Font/Font.h"
#include "Assets/AssetTypes/Font/FontAtlasBaker.h"
#include "Assets/Factories/Factory.h"
#include "Core/Object/Package/Package.h"
#include "Core/Progress/SlowTask.h"
#include "Platform/Filesystem/FileHelper.h"

#include <ft2build.h>
#include "Log/Log.h"
#include FT_FREETYPE_H

namespace Lumina
{
    namespace
    {
        // Pulls display metadata out of the face. Failure is non-fatal; the bytes are still a valid asset,
        // the fields just stay at their defaults.
        void ExtractFontMetadata(CFont* Font)
        {
            FT_Library Library = nullptr;
            if (FT_Init_FreeType(&Library) != 0)
            {
                return;
            }

            FT_Face Face = nullptr;
            if (FT_New_Memory_Face(Library, Font->FontData.data(), (FT_Long)Font->FontData.size(), 0, &Face) == 0)
            {
                Font->FamilyName  = Face->family_name ? Face->family_name : "";
                Font->StyleName   = Face->style_name ? Face->style_name : "";
                Font->NumGlyphs   = (int32)Face->num_glyphs;
                Font->bIsScalable = FT_IS_SCALABLE(Face) != 0;
                Font->bHasKerning = FT_HAS_KERNING(Face) != 0;
                FT_Done_Face(Face);
            }

            FT_Done_FreeType(Library);
        }

        bool LoadFontFromFile(CFont* Font, const FFixedString& SourcePath, FString& OutError)
        {
            TVector<uint8> Bytes;
            if (!FileHelper::LoadFileToArray(Bytes, SourcePath.c_str()) || Bytes.empty())
            {
                OutError = FString("Failed to read font file ") + FString(SourcePath.c_str());
                return false;
            }

            Font->FontData   = Move(Bytes);
            Font->SourcePath = FString(SourcePath.c_str());

            ExtractFontMetadata(Font);

            if (!BakeFontAtlas(Font))
            {
                LOG_WARN("[FontImport] failed to bake MSDF atlas for '{0}'; world-space text won't render with this font",
                         SourcePath.c_str());
            }
            return true;
        }
    }

    void CFontImporter::BuildAssets(const FImportRequest& Request, FImportResult& OutResult, FScopedSlowTask* Progress)
    {
        if (Progress)
        {
            Progress->EnterProgressFrame(0.5f, "Baking font atlas...");
        }

        FFixedString PackagePath = Request.DestinationPath;
        CPackage::AddPackageExt(PackagePath);

        CFont* Font = CFactory::CreateNewOf<CFont>(PackagePath);
        if (Font == nullptr)
        {
            OutResult.Error = FString("A package already exists at ") + FString(PackagePath.c_str());
            return;
        }

        if (!LoadFontFromFile(Font, Request.SourcePath, OutResult.Error))
        {
            Font->ConditionalBeginDestroy();
            return;
        }

        if (Progress)
        {
            Progress->EnterProgressFrame(0.5f, "Saving package...");
        }

        CPackage* Package = Font->GetPackage();
        if (CPackage::SavePackage(Package, Package->GetPackagePath()))
        {
            FAssetRegistry::Get().AssetCreated(Font);
        }
        else
        {
            LOG_ERROR("[FontImport] failed to save '{0}'; asset will not be registered", Package->GetPackagePath());
        }

        OutResult.CreatedObjects.push_back(Font);
    }

    bool CFontImporter::CanReimport(const CStruct* AssetClass) const
    {
        return AssetClass != nullptr && AssetClass->IsChildOf(CFont::StaticClass());
    }

    FString CFontImporter::GetReimportSourcePath(const CObject* Asset) const
    {
        const CFont* Font = Cast<CFont>(Asset);
        return Font != nullptr ? Font->SourcePath : FString();
    }

    bool CFontImporter::ReimportAsset(CObject* Asset, const FImportRequest& Request, FScopedSlowTask* Progress)
    {
        CFont* Font = Cast<CFont>(Asset);
        if (Font == nullptr)
        {
            return false;
        }

        FString Error;
        if (!LoadFontFromFile(Font, Request.SourcePath, Error))
        {
            LOG_ERROR("[FontImport] reimport failed: {0}", Error);
            return false;
        }

        if (CPackage* Package = Font->GetPackage())
        {
            Package->MarkDirty();
        }
        return true;
    }
}
