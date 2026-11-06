#pragma once

#include "Containers/String.h"
#include "Core/Object/Class.h"
#include "Core/Reflection/Type/LuminaTypes.h"

#include "Core/Json/Json.h"

namespace Lumina::Agent
{
    struct FSchemaResult
    {
        FJsonValue Schema;

        // Empty when the whole struct could be expressed. Names the offending property otherwise.
        FString Error;

        NODISCARD bool IsValid() const { return Error.IsEmpty(); }
    };

    // One unsupported field rejects the whole struct, so a tool never advertises a shape it cannot parse.
    AGENTCORE_API FSchemaResult GenerateSchema(CStruct* Struct);

    // The same for a single property, which is what a per-property view of a component needs.
    AGENTCORE_API FSchemaResult GeneratePropertySchema(FProperty* Property);
}
