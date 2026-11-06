#pragma once

#include "Platform/GenericPlatform.h"

// The script schema wire shared by LuminaSharp.Serializer and the native reader, a checked header and length-framed records making a mismatch loud.
namespace Lumina::Scripting
{
    // 'LSCH'. Identifies a schema buffer, so a stale or foreign one is refused rather than parsed.
    inline constexpr uint32 kScriptSchemaMagic = 0x4843534Cu;

    // Bumped only when an existing field changes meaning, type or position, since a reader skips what is appended.
    inline constexpr uint16 kScriptSchemaVersion = 3;

    // 'LTMF'. One script generation's whole type list, written by LuminaSharp.TypeManifest.
    inline constexpr uint32 kTypeManifestMagic = 0x464D544Cu;
    inline constexpr uint16 kTypeManifestVersion = 2;

    // Each record is a tag, a byte length and its payload, so a drift fails by name instead of running off the buffer.
    enum class EScriptSchemaRecord : uint8
    {
        Field = 0,      ///< one [Property] member: name, aliases, metadata, type, default
        Meta,           ///< the editor-facing hints on a field, which is the bag that grows most
        Type,           ///< one FScriptExportType node, recursive
        Function,       ///< one [Function]: its name, its parameters as fields, and its return index
    };
}
