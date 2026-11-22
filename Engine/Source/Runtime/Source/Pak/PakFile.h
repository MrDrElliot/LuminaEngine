#pragma once

#include "Containers/String.h"

namespace Lumina
{
    // The TOC sits last at TocOffset so blobs append without seeking.

    static constexpr uint32 PAK_MAGIC   = 0x4B41504Cu; // 'LPAK'
    static constexpr uint32 PAK_VERSION = 3u;

    // Compression payoff threshold; entries below this are stored raw.
    static constexpr size_t PAK_COMPRESSION_MIN_SIZE = 256;

    enum class EPakCompression : uint8
    {
        None    = 0,
        Deflate = 1,
    };

    struct FPakHeader
    {
        uint32 Magic;
        uint32 Version;
        uint64 TocOffset;
        uint32 EntryCount;
        uint32 _Pad;            // 8-byte alignment
    };
    static_assert(sizeof(FPakHeader) == 24, "FPakHeader on-disk size must be stable");

    // Runtime entry: points into the archive's decompressed buffer.
    struct FPakEntry
    {
        int64 Offset = 0;
        int64 Size   = 0;
    };
}
