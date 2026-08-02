#include "EditorPCH.h"
#include "TextureRenderTargetFactory.h"
#include "Assets/AssetTypes/Textures/TextureRenderTarget.h"
#include "Core/Math/Math.h"
#include "Core/Object/Package/Package.h"
#include "Core/Object/Package/Thumbnail/PackageThumbnail.h"
#include "Renderer/Format.h"
#include "Thumbnails/ThumbnailUtils.h"

namespace Lumina
{
    namespace
    {
        // IEEE 754 binary16 -> binary32 (render targets may be RGBA16_FLOAT).
        FORCEINLINE float HalfToFloat(uint16 H)
        {
            const uint32 Sign = (H & 0x8000u) << 16;
            uint32 Exp        = (H & 0x7C00u) >> 10;
            uint32 Mant       = (H & 0x03FFu);

            uint32 Bits;
            if (Exp == 0)
            {
                if (Mant == 0)
                {
                    Bits = Sign;
                }
                else
                {
                    Exp = 1;
                    while ((Mant & 0x0400u) == 0)
                    {
                        Mant <<= 1;
                        --Exp;
                    }
                    Mant &= 0x03FFu;
                    Bits = Sign | ((Exp + (127 - 15)) << 23) | (Mant << 13);
                }
            }
            else if (Exp == 0x1F)
            {
                Bits = Sign | 0x7F800000u | (Mant << 13);
            }
            else
            {
                Bits = Sign | ((Exp + (127 - 15)) << 23) | (Mant << 13);
            }

            float Out;
            Memory::Memcpy(&Out, &Bits, sizeof(Out));
            return Out;
        }

        // Reads the live GPU image back and writes a 256x256 RGBA8 thumbnail. Contents
        // aren't serialized, so the capture is whatever the target holds (clear color if fresh).
        void GenerateRenderTargetThumbnail(CTextureRenderTarget* RenderTarget)
        {
            // @TODO Render-target thumbnail needs new-RHI readback (CmdCopyTextureToBuffer into a
            // CPURead buffer); the old-RHI staging-image path is retired. No thumbnail for now.
            (void)RenderTarget;
        }
    }

    CObject* CTextureRenderTargetFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        CTextureRenderTarget* RenderTarget = NewObject<CTextureRenderTarget>(Package, Name);
        // Allocate + clear the GPU image now so the asset is sampleable the moment it's created.
        RenderTarget->BuildResource();
        // Capture the freshly-built image so the content browser has a thumbnail instead of the generic icon.
        GenerateRenderTargetThumbnail(RenderTarget);
        return RenderTarget;
    }
}
