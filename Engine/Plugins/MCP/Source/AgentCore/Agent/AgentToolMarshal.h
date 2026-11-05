#pragma once

#include "Containers/String.h"
#include "Core/Object/Class.h"
#include "Core/Reflection/Type/LuminaTypes.h"

#include "Core/Json/Json.h"

namespace Lumina::Agent
{
    struct FMarshalResult
    {
        // Empty on success. Names the offending path otherwise, so a caller can say what to fix.
        FString Error;

        NODISCARD bool IsValid() const { return Error.IsEmpty(); }
    };

    // Fills a struct instance from JSON, refusing anything GenerateSchema would not have advertised.
    AGENTCORE_API FMarshalResult ReadStruct(const FJsonValue& In, CStruct* Type, void* Data);

    // The struct's current values as JSON, in the shape GenerateSchema describes.
    AGENTCORE_API FMarshalResult WriteStruct(CStruct* Type, void* Data, FJsonValue& Out);

    // Checks a value against a property without touching it, so a caller can refuse before transacting.
    AGENTCORE_API FMarshalResult ValidatePropertyValue(const FJsonValue& In, FProperty* Property, FStringView Path);

    // Applies one JSON value to one property instance, refusing whatever ReadStruct would refuse.
    AGENTCORE_API FMarshalResult ReadProperty(const FJsonValue& In, FProperty* Property, void* ValuePtr,
        FStringView Path);

    AGENTCORE_API FMarshalResult WriteProperty(FProperty* Property, void* ValuePtr, FJsonValue& Out);
}
