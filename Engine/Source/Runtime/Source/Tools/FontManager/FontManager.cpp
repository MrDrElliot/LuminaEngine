#include "Core/Threading/Thread.h"
#include "RuntimePCH.h"
#include "FontManager.h"

#include "Assets/AssetTypes/Font/Font.h"
#include "Assets/AssetTypes/Font/FontAtlasBaker.h"
#include "Core/Object/Package/Package.h"
#include "FileSystem/FileSystem.h"
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
        
        // A virtual path, so a packaged game reads it out of its pak.
        const FString Path = "/Engine/Resources/Fonts/Lexend/Lexend-Regular.ttf";

        TVector<uint8> Bytes;
        if (!VFS::ReadFile(Bytes, Path) || Bytes.empty())
        {
            LOG_ERROR("FontManager: failed to load default font '{0}'", Path);
            return;
        }

        DefaultFont->FontData   = Move(Bytes);
        DefaultFont->SourcePath = Path;

        // Uploading at engine init is too early, so the atlas would take an invalid resource id.
        if (!BakeFontAtlas(DefaultFont.Get()))
        {
            LOG_ERROR("FontManager: failed to bake default font atlas from '{0}'", Path);
        }
    }

    CFontManager& CFontManager::Get()
    {
        static FOnceFlag Flag;
        CallOnce(Flag, []()
        {
            FontManagerSingleton = NewObject<CFontManager>();
            FontManagerSingleton->Initialize();
            FontManagerSingleton->AddToRoot();
        });

        return *FontManagerSingleton;
    }
}
