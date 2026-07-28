#include "pch.h"
#include "FontFactory.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Font/FontAtlasBaker.h"
#include "Core/Object/Package/Package.h"
#include "Platform/Filesystem/FileHelper.h"

#include <ft2build.h>
#include "Log/Log.h"
#include FT_FREETYPE_H

namespace Lumina
{
    CObject* CFontFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        return NewObject<CFont>(Package, Name);
    }

    // Pulls display metadata out of the face. Failure is non-fatal; the bytes are
    // still a valid asset, the fields just stay at their defaults.
    static void ExtractFontMetadata(CFont* Font)
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

    void CFontFactory::TryImport(const FFixedString& RawPath, const FFixedString& DestinationPath, const Import::FImportSettings* Settings)
    {
        TVector<uint8> Bytes;
        if (!FileHelper::LoadFileToArray(Bytes, RawPath.c_str()) || Bytes.empty())
        {
            LOG_ERROR("FontFactory: failed to read font file '{0}'", RawPath.c_str());
            return;
        }

        CFont* NewFont = TryCreateNew<CFont>(DestinationPath);
        NewFont->FontData = Move(Bytes);
        NewFont->SourcePath = FString(RawPath.c_str());

        ExtractFontMetadata(NewFont);

        if (!BakeFontAtlas(NewFont))
        {
            LOG_WARN("FontFactory: failed to bake MSDF atlas for '{0}'; world-space text won't render with this font", RawPath.c_str());
        }

        CPackage* NewPackage = NewFont->GetPackage();
        if (CPackage::SavePackage(NewPackage, NewPackage->GetPackagePath()))
        {
            FAssetRegistry::Get().AssetCreated(NewFont);
        }
        else
        {
            LOG_ERROR("FontFactory: failed to save imported font; asset will not be registered");
        }

        NewFont->ConditionalBeginDestroy();
    }
}
