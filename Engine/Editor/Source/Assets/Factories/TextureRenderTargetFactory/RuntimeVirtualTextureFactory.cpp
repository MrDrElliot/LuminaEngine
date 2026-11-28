#include "EditorPCH.h"
#include "RuntimeVirtualTextureFactory.h"

namespace Lumina
{
    CObject* CRuntimeVirtualTextureFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        CRuntimeVirtualTexture* Texture = NewObject<CRuntimeVirtualTexture>(Package, Name);
        Texture->Width  = 1024;
        Texture->Height = 1024;
        Texture->BuildResource();
        return Texture;
    }
}
