#pragma once
#include "Containers/Vector.h"
#include "Platform/GenericPlatform.h"

// stb_image_write is compiled only in StbImageImpl.cpp, so these exports are how anything outside Runtime reaches it.

namespace Lumina::ImageWrite
{
    /// PNG-encodes tightly packed pixels into memory. Returns false and leaves Out untouched on failure.
    RUNTIME_API bool EncodePng(
        TVector<uint8>& Out, uint32 Width, uint32 Height, uint32 Channels, const uint8* Pixels, uint32 RowPitch);

    /// PNG-encodes pixels straight to a file.
    RUNTIME_API bool WritePngFile(
        const char* Path, uint32 Width, uint32 Height, uint32 Channels, const uint8* Pixels, uint32 RowPitch);

    /// Radiance HDR-encodes float pixels straight to a file.
    RUNTIME_API bool WriteHdrFile(
        const char* Path, uint32 Width, uint32 Height, uint32 Channels, const float* Pixels);
}
