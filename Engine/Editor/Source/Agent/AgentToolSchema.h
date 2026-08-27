#pragma once

#include "Containers/String.h"
#include "Core/Object/Class.h"
#include "Core/Reflection/Type/LuminaTypes.h"

#include "nlohmann/json.hpp"

namespace Lumina::Agent
{
    struct FSchemaResult
    {
        nlohmann::json Schema;

        // Empty when the whole struct could be expressed. Names the offending property otherwise.
        FString Error;

        NODISCARD bool IsValid() const { return Error.empty(); }
    };

    // One unsupported field rejects the whole struct, so a tool never advertises a shape it cannot parse.
    EDITOR_API FSchemaResult GenerateSchema(CStruct* Struct);

    // The same for a single property, which is what a per-property view of a component needs.
    EDITOR_API FSchemaResult GeneratePropertySchema(FProperty* Property);
}
