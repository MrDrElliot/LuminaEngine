#include "RuntimePCH.h"

#include "ImageWrite.h"
#include "Memory/Memory.h"

// A buffer handed back to stb is freed by the same allocator that produced it.

// Memory::Free takes its pointer by reference to null it, and stb frees rvalue expressions.
#define STBI_MALLOC(Size)              LmThirdPartyMalloc(Size, "stb_image")
#define STBI_REALLOC(Ptr, NewSize)     LmThirdPartyRealloc(Ptr, NewSize, "stb_image")
#define STBI_FREE(Ptr)                 LmThirdPartyFree(Ptr)

#define STBIW_MALLOC(Size)             LmThirdPartyMalloc(Size, "stb_image_write")
#define STBIW_REALLOC(Ptr, NewSize)    LmThirdPartyRealloc(Ptr, NewSize, "stb_image_write")
#define STBIW_FREE(Ptr)                LmThirdPartyFree(Ptr)

#define STBIR_MALLOC(Size, Context)    LmThirdPartyMalloc(Size, "stb_image_resize")
#define STBIR_FREE(Ptr, Context)       LmThirdPartyFree(Ptr)

#include "miniz.h"

// stb's own deflate is several times slower than miniz at its fastest level, which made a full-screen PNG cost most of a second.
static unsigned char* LuminaStbiwZlibCompress(unsigned char* Data, int DataLength, int* OutLength, int /*Quality*/)
{
    mz_ulong Bound = mz_compressBound((mz_ulong)DataLength);
    unsigned char* Out = static_cast<unsigned char*>(LmThirdPartyMalloc(Bound, "stb_image_write"));
    if (Out == nullptr)
    {
        return nullptr;
    }
    if (mz_compress2(Out, &Bound, Data, (mz_ulong)DataLength, MZ_BEST_SPEED) != MZ_OK)
    {
        LmThirdPartyFree(Out);
        return nullptr;
    }
    *OutLength = (int)Bound;
    return Out;
}
#define STBIW_ZLIB_COMPRESS LuminaStbiwZlibCompress

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"
#include "stb_image_resize2.h"

namespace Lumina::ImageWrite
{
    namespace
    {
        void AppendEncodedBytes(void* Context, void* Data, int Size)
        {
            auto* Out = static_cast<TVector<uint8>*>(Context);
            const uint8* Bytes = static_cast<const uint8*>(Data);
            Out->insert(Out->end(), Bytes, Bytes + Size);
        }
    }

    bool EncodePng(
        TVector<uint8>& Out, uint32 Width, uint32 Height, uint32 Channels, const uint8* Pixels, uint32 RowPitch)
    {
        TVector<uint8> Encoded;

        const int Ok = stbi_write_png_to_func(&AppendEncodedBytes, &Encoded,
            (int)Width, (int)Height, (int)Channels, Pixels, (int)RowPitch);

        if (Ok == 0 || Encoded.empty())
        {
            return false;
        }

        Out = std::move(Encoded);
        return true;
    }

    bool WritePngFile(
        const char* Path, uint32 Width, uint32 Height, uint32 Channels, const uint8* Pixels, uint32 RowPitch)
    {
        // stb otherwise tries all five row filters on every row, and Paeth alone suits rendered images.
        constexpr int PaethFilter = 4;
        stbi_write_force_png_filter = PaethFilter;
        return stbi_write_png(Path, (int)Width, (int)Height, (int)Channels, Pixels, (int)RowPitch) != 0;
    }

    bool WriteHdrFile(const char* Path, uint32 Width, uint32 Height, uint32 Channels, const float* Pixels)
    {
        return stbi_write_hdr(Path, (int)Width, (int)Height, (int)Channels, Pixels) != 0;
    }
}
