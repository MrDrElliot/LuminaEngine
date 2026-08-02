#include "RuntimePCH.h"
#include "FontManager.h"

#include "Assets/AssetTypes/Font/Font.h"
#include "Assets/AssetTypes/Font/FontAtlasBaker.h"
#include "Core/Object/Package/Package.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/FileHelper.h"
#include "Log/Log.h"

namespace Lumina
{
    static CFontManager* FontManagerSingleton = nullptr;

    CFontManager::CFontManager()
    {
    }

    void CFontManager::Initialize()
    {
        // Transient package with a stable GUID so worlds reference it across save/load without a disk file.
        CPackage* TransientPackage = CPackage::GetTransientPackage();

        DefaultFont = NewObject<CFont>(TransientPackage, "EngineDefaultFont", FGuid::NewDeterministic("Engine.Font.Default"));
        
        const FString Path = Paths::GetEngineFontDirectory() + "/Lexend/Lexend-Regular.ttf";

        TVector<uint8> Bytes;
        if (!FileHelper::LoadFileToArray(Bytes, Path) || Bytes.empty())
        {
            LOG_ERROR("FontManager: failed to load default font '{0}'", Path);
            return;
        }

        DefaultFont->FontData   = Move(Bytes);
        DefaultFont->SourcePath = Path;

        // Bake the glyph atlas now (CPU only). The GPU image is uploaded lazily on first render (the extract
        // calls GetAtlasImage) -- uploading here at engine init is too early: the bindless texture table
        // isn't ready, so the atlas would get an invalid resource id and never draw.
        if (!BakeFontAtlas(DefaultFont))
        {
            LOG_ERROR("FontManager: failed to bake default font atlas from '{0}'", Path);
        }
    }

    CFontManager& CFontManager::Get()
    {
        static std::once_flag Flag;
        std::call_once(Flag, []()
        {
            FontManagerSingleton = NewObject<CFontManager>();
            FontManagerSingleton->Initialize();
            FontManagerSingleton->AddToRoot();
        });

        return *FontManagerSingleton;
    }
}
