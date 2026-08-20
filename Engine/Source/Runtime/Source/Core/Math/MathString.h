#pragma once

#include "Containers/StringFormat.h"
#include "Vector/VectorTypes.h"
#include "Quat/Quat.h"

// Stringify helpers for the math types (debug / property display).

namespace Lumina::Math
{
    template<typename T>
    [[nodiscard]] inline FString ToString(const TVec<T, 2>& V)
    {
        return Format("({}, {})", V.x, V.y);
    }

    template<typename T>
    [[nodiscard]] inline FString ToString(const TVec<T, 3>& V)
    {
        return Format("({}, {}, {})", V.x, V.y, V.z);
    }

    template<typename T>
    [[nodiscard]] inline FString ToString(const TVec<T, 4>& V)
    {
        return Format("({}, {}, {}, {})", V.x, V.y, V.z, V.w);
    }

    template<typename T>
    [[nodiscard]] inline FString ToString(const TQuat<T>& Q)
    {
        return Format("(w={}, {}, {}, {})", Q.w, Q.x, Q.y, Q.z);
    }
}
