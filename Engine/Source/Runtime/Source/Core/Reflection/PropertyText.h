#pragma once

#include "Containers/String.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class FProperty;
}

// Text form of a single reflected property value: what a CSV cell holds, and what a compact grid cell
// shows and edits.
//
// Scalars, bools, strings, names, enums and object references round-trip exactly. Aggregates -- structs,
// containers, delegates -- deliberately do NOT. A CSV cell is the wrong place to encode them, and a
// half-working parse that silently drops fields is worse than refusing outright. Ask IsTextConvertible
// before offering a column as editable.
namespace Lumina::Reflection
{
    RUNTIME_API bool IsTextConvertible(const FProperty* Property);

    // Empty string for a null object reference. Returns "" for a non-convertible property rather than
    // inventing a representation that FromText could not read back.
    RUNTIME_API FString ToText(const FProperty* Property, const void* Container);

    // Leaves the value untouched and returns false on a malformed number, an unknown enum name, an
    // unresolvable object path, or a non-convertible property.
    RUNTIME_API bool FromText(const FProperty* Property, void* Container, FStringView Text);
}
