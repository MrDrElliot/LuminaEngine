#include "DotNetExport.h"
#include "ExportSignature.h"
#include "Core/Serialization/NetArchive.h"

// The bit archive a C# struct's NetSerialize writes and reads, handed over as an opaque pointer for the duration of one NetSerialize.

namespace Lumina::DotNetNetArchiveInternal
{
    FORCEINLINE FNetArchive& AsArchive(void* Archive) { return *static_cast<FNetArchive*>(Archive); }
}

LUMINA_DOTNET_EXPORT(int, NetArchiveIsReading)(void* Archive)
{
    return Lumina::DotNetNetArchiveInternal::AsArchive(Archive).IsReading() ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(int, NetArchiveHasError)(void* Archive)
{
    return Lumina::DotNetNetArchiveInternal::AsArchive(Archive).HasError() ? 1 : 0;
}

LUMINA_DOTNET_EXPORT(void, NetArchiveSetError)(void* Archive)
{
    Lumina::DotNetNetArchiveInternal::AsArchive(Archive).SetHasError(true);
}

// The low NumBits of Value, which a little-endian uint64 lays out first in memory.
LUMINA_DOTNET_EXPORT(void, NetArchiveBits)(void* Archive, Lumina::uint64* Value, Lumina::uint32 NumBits)
{
    Lumina::DotNetNetArchiveInternal::AsArchive(Archive).SerializeBits(Value, NumBits <= 64u ? NumBits : 64u);
}

LUMINA_DOTNET_EXPORT(void, NetArchiveBytes)(void* Archive, void* Data, Lumina::int64 Size)
{
    Lumina::DotNetNetArchiveInternal::AsArchive(Archive).Serialize(Data, Size);
}

LUMINA_DOTNET_EXPORT(void, NetArchiveEntity)(void* Archive, Lumina::uint32* Entity)
{
    Lumina::DotNetNetArchiveInternal::AsArchive(Archive).SerializeEntity(*Entity);
}

LUMINA_DOTNET_SIGNATURES(
    LUMINA_DOTNET_SIG(NetArchiveIsReading),
    LUMINA_DOTNET_SIG(NetArchiveHasError),
    LUMINA_DOTNET_SIG(NetArchiveSetError),
    LUMINA_DOTNET_SIG(NetArchiveBits),
    LUMINA_DOTNET_SIG(NetArchiveBytes),
    LUMINA_DOTNET_SIG(NetArchiveEntity)
);
