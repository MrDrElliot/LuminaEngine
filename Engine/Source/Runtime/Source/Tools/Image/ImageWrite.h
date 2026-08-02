#pragma once
#include "Containers/Array.h"
#include "Platform/GenericPlatform.h"

// Image encoding for the whole engine. stb_image_write is a header-only library whose
// implementation must be compiled in exactly one translation unit per image; Runtime and Editor
// each used to compile their own, which works only for as long as they stay separate DLLs and
// breaks a monolithic link with duplicate symbols. StbImageImpl.cpp is now the single owner, and
// these exported entry points are how anything outside Runtime reaches it.

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
