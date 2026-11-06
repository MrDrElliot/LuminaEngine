#pragma once

#include "Containers/Vector.h"
#include "ScriptExports.h"
#include "ScriptSchemaWire.h"

namespace Lumina::Scripting
{
    // Reads the schema wire outside the .NET host, so the codec can be tested without booting a runtime.

    // Decodes a managed-written schema buffer. False on a bad header, a truncation, or a record desync.
    RUNTIME_API bool ParseSchemaBlob(const TVector<uint8>& Blob, FScriptExportSchema& OutSchema);

    struct FManagedTypeDefinition;
    struct FManagedTypeAlias;

    // Decodes the one blob a script generation describes itself with. False on a bad header or a truncation.
    RUNTIME_API bool ParseTypeManifest(const TVector<uint8>& Blob, TVector<FManagedTypeDefinition>& OutTypes,
        TVector<FManagedTypeAlias>& OutAliases);
}
