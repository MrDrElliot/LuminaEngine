#include "RuntimePCH.h"

#include "ImageWrite.h"
#include "Memory/Memory.h"

// The one translation unit that compiles stb's implementations. Every other file includes the stb
// headers for declarations only. Keeping the implementations together also keeps the allocator
// hooks in one place: stb allocates through the engine allocator so its buffers show up in memory
// tracking like everything else, and so a buffer handed back to stb is freed by the same allocator
// that produced it.

// The LmThirdParty shims rather than Memory::Malloc directly: Memory::Free takes a pointer by
// reference so it can null the caller's variable, and stb frees rvalue expressions.
#define STBI_MALLOC(Size)              LmThirdPartyMalloc(Size, "stb_image")
#define STBI_REALLOC(Ptr, NewSize)     LmThirdPartyRealloc(Ptr, NewSize, "stb_image")
#define STBI_FREE(Ptr)                 LmThirdPartyFree(Ptr)

#define STBIW_MALLOC(Size)             LmThirdPartyMalloc(Size, "stb_image_write")
#define STBIW_REALLOC(Ptr, NewSize)    LmThirdPartyRealloc(Ptr, NewSize, "stb_image_write")
#define STBIW_FREE(Ptr)                LmThirdPartyFree(Ptr)

#define STBIR_MALLOC(Size, Context)    LmThirdPartyMalloc(Size, "stb_image_resize")
#define STBIR_FREE(Ptr, Context)       LmThirdPartyFree(Ptr)

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
        return stbi_write_png(Path, (int)Width, (int)Height, (int)Channels, Pixels, (int)RowPitch) != 0;
    }

    bool WriteHdrFile(const char* Path, uint32 Width, uint32 Height, uint32 Channels, const float* Pixels)
    {
        return stbi_write_hdr(Path, (int)Width, (int)Height, (int)Channels, Pixels) != 0;
    }
}
