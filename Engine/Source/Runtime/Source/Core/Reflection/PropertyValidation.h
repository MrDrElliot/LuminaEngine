#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"

namespace Lumina
{
    class CStruct;
    class FProperty;
}

// A property opts in with PROPERTY(EditValidate = "Fn"), naming a FUNCTION on its owner that takes one value of the property's type and returns why it is refused.
namespace Lumina::PropertyValidation
{
    // Why Candidate, a value of the property's type, cannot go into Property on the object or struct at Container. Empty when it can or nothing validates it.
    RUNTIME_API FString Check(const FProperty* Property, const void* Container, const void* Candidate);

    // The same for the value Property holds now, so a field that was set before its rule existed still shows the problem.
    RUNTIME_API FString CheckCurrent(const FProperty* Property, const void* Container);

    struct FPropertyError
    {
        const FProperty* Property = nullptr;
        FString          Reason;
    };

    // Every property of Type whose held value on Container is refused, for flagging the object itself rather than one field.
    RUNTIME_API TVector<FPropertyError> CollectErrors(const CStruct* Type, const void* Container);
}
