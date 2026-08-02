#pragma once
#include "Core/Object/ObjectMacros.h"
#include "Containers/String.h"
#include "GUID/GUID.h"
#include "AssetRef.generated.h"

namespace Lumina
{
    class FNetArchive;

    // Rename-safe reference to a loose text asset (.luau/.rml/.rcss).
    REFLECT()
    struct RUNTIME_API FAssetRef
    {
        GENERATED_BODY()

        /** Last-known virtual path, e.g. "/Scripts/Player.luau". Healed on resolve after a rename. */
        PROPERTY(Editable)
        FString Path;

        /** Stable identity (GUID text). Serialized but not user-editable; the picker maintains it. */
        PROPERTY()
        FString Guid;

        FAssetRef() = default;
        explicit FAssetRef(FStringView InPath) : Path(InPath.data(), InPath.size()) {}

        bool IsNull() const  { return Path.empty() && Guid.empty(); }
        bool IsValid() const { return !IsNull(); }

        FStringView GetPath() const { return FStringView(Path.c_str(), Path.size()); }
        FGuid       GetGuid() const;

        /** GUID-first resolution against the text-asset registry. Heals Path in place if the GUID points at
         *  a renamed file; otherwise back-fills the GUID from the current Path. Pure lookup (no file I/O,
         *  no minting) so it is safe on any thread and in cooked builds. Returns the current path. */
        FStringView ResolvePath() const;

        /** Resolve, then read the file's text. False on miss. */
        bool ReadText(FString& OutText) const;

        /** Editor-side assignment. Sets the path and (re)derives the GUID, minting a sidecar if needed. */
        void SetPath(FStringView InPath);
        void Set(FStringView InPath, const FGuid& InGuid);
        void Reset() { Path.clear(); Guid.clear(); }

        /** Tight network serialization. With the archive's asset-index hooks bound, writes a compact varint
         *  index (Path+GUID exported once via ENetMessage::AssetExport); otherwise the full Path + GUID. */
        void NetSerialize(FNetArchive& Ar);

        bool operator==(const FAssetRef& Other) const { return Path == Other.Path && Guid == Other.Guid; }
        bool operator!=(const FAssetRef& Other) const { return !(*this == Other); }
    };
}
