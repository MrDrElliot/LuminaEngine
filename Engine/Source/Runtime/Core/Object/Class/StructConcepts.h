#pragma once

#include "Containers/String.h"

namespace Lumina
{
    struct FPropertyChangedEvent;
    class FArchive;
    class FNetArchive;
}

namespace Lumina::Concepts
{
    template<typename T>
    concept THasSerialize = requires(T& V, FArchive& Ar)
    {
        { V.Serialize(Ar) } -> std::same_as<bool>;
    };

    // A type opting into custom/tight network serialization: a dedicated NetSerialize that takes the
    // bit archive (distinct from the disk Serialize above). Lets math types quantize on the wire.
    template<typename T>
    concept THasNetSerialize = requires(T& V, FNetArchive& Ar)
    {
        { V.NetSerialize(Ar) } -> std::same_as<void>;
    };
    
    template<typename T>
    concept THasCopy = requires(T& Dst, const T& Src)
    {
        { Dst.CopyFrom(Src) } -> std::same_as<void>;
    };
    
    template<typename T>
    concept THasEquality = requires(const T& A, const T& B)
    {
        { A == B } -> std::same_as<bool>;
    };
    
    template<typename T>
    concept THasToString = requires(const T& V)
    {
        { V.ToString() } -> std::same_as<FString>;
    };
    
    template<typename T>
    concept THasLessThan = requires(const T& A, const T& B)
    {
        { A < B } -> std::same_as<bool>;
    };
    
    #if USING(WITH_EDITOR)
    
    // Non-const on purpose: reacting to an edit means mutating -- invalidating a cache, clamping a
    // value, recomputing derived state. Requiring a const hook would force every implementation into
    // mutable members or a const_cast, and because these are `if constexpr` tested, a non-const hook
    // silently fails the concept and is never wired up rather than failing to compile.
    template<typename T>
    concept THasPreEdit = requires(T& A, const FPropertyChangedEvent& PropertyEvent)
    {
        { A.PreEditChange(PropertyEvent) } -> std::same_as<void>;
    };
    
    template<typename T>
    concept THasPostEdit = requires(T& A, const FPropertyChangedEvent& PropertyEvent)
    {
        { A.PostEditChange(PropertyEvent) } -> std::same_as<void>;
    };
    
    #endif
}
