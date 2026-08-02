#pragma once

#include "Containers/Array.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "GUID/GUID.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    // Asset dependency edge kinds; the cooker honors these to decide traversal and shipping.
    enum class EDependencyType : uint8
    {
        // Always followed; target lands in the referrer's chunk (or one reachable from it).
        Hard         = 0,

        // Optional/async ref; followed only to mark reachable, chunk decided by target's own roots.
        Soft         = 1,

        // Discovered via script analysis; treated like Soft at cook time.
        Script       = 2,

        // Referrer owns target; forced into the same chunk, orphan delete needs parent ack.
        Owned        = 3,

        // Editor tooling reference (preview, thumbnail). Pruned in cook.
        EditorOnly   = 4,

        // Generated at runtime; recorded for diagnostics but not followed by cook.
        Generated    = 5,
    };

    RUNTIME_API FStringView LexToString(EDependencyType T);

    // Per-asset cook flags; from REFLECT(Class, ...) metadata + per-asset overrides in the package header.
    enum class EAssetFlags : uint32
    {
        None         = 0,
        EditorOnly   = 1 << 0,  // Never enters cooked output.
        RuntimeOnly  = 1 << 1,  // Skipped during editor-mode registration.
        AlwaysCook   = 1 << 2,  // Implicit root; included even without inbound ref.
        NeverCook    = 1 << 3,  // Forbid cooking; report error if reached.
        Primary      = 1 << 4,  // Auto-root for Asset Manager (Phase 2).
    };

    constexpr EAssetFlags operator|(EAssetFlags a, EAssetFlags b)
    {
        return static_cast<EAssetFlags>(static_cast<uint32>(a) | static_cast<uint32>(b));
    }
    constexpr EAssetFlags operator&(EAssetFlags a, EAssetFlags b)
    {
        return static_cast<EAssetFlags>(static_cast<uint32>(a) & static_cast<uint32>(b));
    }
    constexpr bool HasFlag(EAssetFlags Set, EAssetFlags F)
    {
        return (static_cast<uint32>(Set) & static_cast<uint32>(F)) != 0;
    }

    // One outbound edge; direct refs only (transitive closure is FCookGraph's job at cook time).
    struct FAssetDependency
    {
        FGuid           TargetGUID;
        EDependencyType Type = EDependencyType::Hard;

        bool operator==(const FAssetDependency& Other) const
        {
            return TargetGUID == Other.TargetGUID && Type == Other.Type;
        }
    };

    struct FAssetData
    {
        FGuid                       AssetGUID;
        FFixedString                Path;
        FName                       AssetName;
        FName                       AssetClass;

        // xxhash of raw .lasset bytes; skips re-extraction on unchanged files + cooker input fingerprint.
        uint64                      ContentHash    = 0;

        // Source mtime (unix-epoch nanos); cheap pre-check before hashing on each discovery pass.
        int64                       SourceMTimeNs  = 0;

        EAssetFlags                 Flags          = EAssetFlags::None;

        // Direct outbound refs from the package ImportTable (reflection-typed where possible).
        TVector<FAssetDependency>   Dependencies;

        // Default-chunk hint from the owning plugin/project.
        FName                       OwnerChunk;

        // Plugin name for /<PluginName>/* content (empty for /Game and /Engine).
        FName                       OwningPlugin;
    };
}
