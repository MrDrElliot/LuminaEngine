#pragma once

#include "Containers/Vector.h"
#include "ScriptExports.h"
#include "ScriptSchemaWire.h"

namespace Lumina::Scripting
{
    /**
     * Reader and writer for the script schema wire, kept next to the contract it implements rather than
     * inside the .NET host: the host is where the bytes ARRIVE, but the shape of them is this file's job, and
     * a codec that cannot be reached without booting the runtime cannot be tested either.
     */

    /** Decodes a managed-written schema buffer. False on a bad header, a truncation, or a record desync. */
    RUNTIME_API bool ParseSchemaBlob(const TVector<uint8>& Blob, FScriptExportSchema& OutSchema,
        TVector<FScriptPropertyEntry>& OutDefaults);

    /** Decodes the [Button] list managed writes. A flat wire of its own, framed by nothing but its count. */
    RUNTIME_API void ParseButtonsBlob(const TVector<uint8>& Blob, TVector<FScriptButton>& OutButtons);

    /** Encodes one value the way the managed reader expects, for pushing overrides onto a live instance. */
    RUNTIME_API void WriteScriptValue(TVector<uint8>& OutBytes, const FScriptPropertyValue& Value);
}
