#pragma once

#include "Vector/VectorTypes.h"
#include "Quat/Quat.h"
#include <string>

// Stringify helpers for the math types (debug / property display).

namespace Lumina::Math
{
    template<typename T>
    [[nodiscard]] inline std::string ToString(const TVec<T, 2>& V)
    {
        return "(" + std::to_string(V.x) + ", " + std::to_string(V.y) + ")";
    }

    template<typename T>
    [[nodiscard]] inline std::string ToString(const TVec<T, 3>& V)
    {
        return "(" + std::to_string(V.x) + ", " + std::to_string(V.y) + ", " + std::to_string(V.z) + ")";
    }

    template<typename T>
    [[nodiscard]] inline std::string ToString(const TVec<T, 4>& V)
    {
        return "(" + std::to_string(V.x) + ", " + std::to_string(V.y) + ", " + std::to_string(V.z) + ", " + std::to_string(V.w) + ")";
    }

    template<typename T>
    [[nodiscard]] inline std::string ToString(const TQuat<T>& Q)
    {
        return "(w=" + std::to_string(Q.w) + ", " + std::to_string(Q.x) + ", " + std::to_string(Q.y) + ", " + std::to_string(Q.z) + ")";
    }
}
