#pragma once

#include "Containers/String.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class FProperty;
}

// Text form of a property value as held by a CSV cell. Structs, maps and delegates are refused rather than half parsed.
namespace Lumina::Reflection
{
    RUNTIME_API bool IsTextConvertible(const FProperty* Property);

    // Returns an empty string for a null reference or a non-convertible property.
    RUNTIME_API FString ToText(const FProperty* Property, const void* Container);

    // Leaves the value untouched and returns false on malformed or unresolvable text.
    RUNTIME_API bool FromText(const FProperty* Property, void* Container, FStringView Text);
}
