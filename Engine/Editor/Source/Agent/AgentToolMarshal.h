#pragma once

#include "Containers/String.h"
#include "Core/Object/Class.h"
#include "Core/Reflection/Type/LuminaTypes.h"

#include "nlohmann/json.hpp"

namespace Lumina::Agent
{
    struct FMarshalResult
    {
        // Empty on success. Names the offending path otherwise, so a caller can say what to fix.
        FString Error;

        NODISCARD bool IsValid() const { return Error.empty(); }
    };

    // Fills a struct instance from JSON, refusing anything GenerateSchema would not have advertised.
    EDITOR_API FMarshalResult ReadStruct(const nlohmann::json& In, CStruct* Type, void* Data);

    // The struct's current values as JSON, in the shape GenerateSchema describes.
    EDITOR_API FMarshalResult WriteStruct(CStruct* Type, void* Data, nlohmann::json& Out);

    // Checks a value against a property without touching it, so a caller can refuse before transacting.
    EDITOR_API FMarshalResult ValidatePropertyValue(const nlohmann::json& In, FProperty* Property, FStringView Path);

    // Applies one JSON value to one property instance, refusing whatever ReadStruct would refuse.
    EDITOR_API FMarshalResult ReadProperty(const nlohmann::json& In, FProperty* Property, void* ValuePtr,
        FStringView Path);

    EDITOR_API FMarshalResult WriteProperty(FProperty* Property, void* ValuePtr, nlohmann::json& Out);
}
