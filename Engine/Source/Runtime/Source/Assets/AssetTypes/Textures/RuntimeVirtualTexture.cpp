#include "RuntimePCH.h"
#include "RuntimeVirtualTexture.h"

namespace Lumina
{
    void CRuntimeVirtualTexture::PostLoad()
    {
        Height = Width;
        CTextureRenderTarget::PostLoad();
    }

    // Square, since the window wraps the same way along both axes.
    void CRuntimeVirtualTexture::PostPropertyChange(FProperty* ChangedProperty)
    {
        CTextureRenderTarget::PostPropertyChange(ChangedProperty);
        Width  = Math::Clamp(Width, 16u, 8192u);
        Height = Width;
        BuildResource();
    }
}
