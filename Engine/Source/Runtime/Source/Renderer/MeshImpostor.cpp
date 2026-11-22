#include "RuntimePCH.h"
#include "MeshImpostor.h"
#include "TaskSystem/TaskSystem.h"
#include "transcoder/basisu_transcoder_internal.h"

namespace Lumina
{
    namespace MeshImpostorInternal
    {
        constexpr uint32 kBlockBytes = 16;

        // Mode 7 is left out, since basisu's two-subset alpha packer divides by zero on some coverage edges.
        constexpr uint32 kPackFlags = basist::bc7f::cPackBC7FlagDefault & ~basist::bc7f::cPackBC7FlagUse2SubsetsRGBA;

        TVector<uint8> CompressAtlas(const FMeshImpostor& Source, const TVector<uint8>& Texels, uint32 MipCount)
        {
            const uint32 Atlas = Source.GetAtlasSize();
            uint64 Bytes = 0;
            for (uint32 Mip = 0; Mip < MipCount; ++Mip)
            {
                const uint64 Side = Atlas >> Mip;
                Bytes += Side * Side;
            }

            TVector<uint8> Out;
            Out.SetNumZeroed((int32)Bytes);
            uint64 WriteOffset = 0;
            for (uint32 Mip = 0; Mip < MipCount; ++Mip)
            {
                const uint32 Side    = Atlas >> Mip;
                const uint32 Blocks  = Side / FMeshImpostor::kBlockSide;
                const uint8* Rows    = Texels.GetData() + Source.GetMipOffset(Mip);
                uint8*       Encoded = Out.GetData() + WriteOffset;
                Jobs::ParallelForOrSerial(Blocks, 4u, [&](uint32 By)
                {
                    basist::color_rgba Pixels[16];
                    for (uint32 Bx = 0; Bx < Blocks; ++Bx)
                    {
                        for (uint32 Y = 0; Y < FMeshImpostor::kBlockSide; ++Y)
                        {
                            const uint8* Row = Rows + ((uint64)(By * FMeshImpostor::kBlockSide + Y) * Side + Bx * FMeshImpostor::kBlockSide) * 4u;
                            Memory::Memcpy(&Pixels[Y * FMeshImpostor::kBlockSide], Row, FMeshImpostor::kBlockSide * 4u);
                        }
                        basist::bc7f::fast_pack_bc7_auto_rgba(Encoded + ((uint64)By * Blocks + Bx) * kBlockBytes, Pixels, kPackFlags);
                    }
                }, 1);
                WriteOffset += (uint64)Side * Side;
            }
            return Out;
        }
    }

    void Impostor::Compress(FMeshImpostor& Impostor)
    {
        if (Impostor.bCompressed || !Impostor.IsValid() || Impostor.FrameSize < FMeshImpostor::kBlockSide)
        {
            return;
        }

        LUMINA_PROFILE_SCOPE();

        FMeshImpostor Compressed = Impostor;
        Compressed.bCompressed = true;
        const uint32 MipCount  = Compressed.GetMipCount();
        Compressed.BaseColor   = MeshImpostorInternal::CompressAtlas(Impostor, Impostor.BaseColor, MipCount);
        Compressed.NormalDepth = MeshImpostorInternal::CompressAtlas(Impostor, Impostor.NormalDepth, MipCount);
        Impostor = Move(Compressed);
    }
}
