#pragma once

#include "Containers/String.h"
#include "Containers/StringView.h"
#include "Core/Object/Class.h"
#include "Core/Object/Object.h"

#include "Core/Json/Json.h"

namespace Lumina::Agent
{
    struct FObjectEditResult
    {
        /// JSON text of the field before and after, so a caller can report both without a second read.
        FString Previous;
        FString Current;

        FString Error;

        /// True when an open editor recorded the edit, so undo can reach it.
        bool bUndoable = false;

        NODISCARD bool IsValid() const { return Error.IsEmpty(); }
    };

    /// Sets one field by dotted path on data owned by Owner, transacted through its open editor when there is one.
    NODISCARD AGENTCORE_API FObjectEditResult SetObjectProperty(CStruct* Root, void* RootData, CObject* Owner,
        FStringView Path, const FJsonValue& Value, FName Label);

    /// Property paths whose type the marshaler cannot write, so a caller can list them before trying.
    AGENTCORE_API void CollectUnwritableFields(CStruct* Root, FStringView Prefix, TVector<FString>& OutPaths);
}
