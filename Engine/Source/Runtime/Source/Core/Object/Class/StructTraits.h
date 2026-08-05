#pragma once

#include "StructConcepts.h"
#include "Containers/String.h"


namespace Lumina
{
    struct FPropertyChangedEvent;
    class FArchive;
    class FNetArchive;

    struct FStructOps
    {
        using SerializeFn    = bool(*)(FArchive&, void*);
        using NetSerializeFn = void(*)(FNetArchive&, void*);
        using CopyFn        = void(*)(void*, const void*);
        using EqualsFn      = bool(*)(const void*, const void*);
        using ToStringFn    = FString(*)(const void*);
        using LessThanFn    = bool(*)(const void*, const void*);
        using ConstructFn   = void(*)(void*);
        using DestructFn    = void(*)(void*);
        
        #if USING(WITH_EDITOR)
        using PreEditFn     = void(*)(void*, const FPropertyChangedEvent&);
        using PostEditFn    = void(*)(void*, const FPropertyChangedEvent&);
        #endif

        SerializeFn     Serialize    = nullptr;
        NetSerializeFn  NetSerialize = nullptr;
        CopyFn          Copy        = nullptr;
        EqualsFn        Equals      = nullptr;
        ToStringFn      ToString    = nullptr;
        LessThanFn      LessThan    = nullptr;
        ConstructFn     Construct   = nullptr;
        DestructFn      Destruct    = nullptr;
        #if USING(WITH_EDITOR)
        PreEditFn       PreEdit     = nullptr;
        PostEditFn      PostEdit    = nullptr;
        #endif

        bool HasSerializer()    const { return Serialize    != nullptr; }
        bool HasNetSerializer() const { return NetSerialize != nullptr; }
        bool HasCopy()          const { return Copy         != nullptr; }
        bool HasEquality()      const { return Equals       != nullptr; }
        bool HasToString()      const { return ToString     != nullptr; }
        bool HasLessThan()      const { return LessThan     != nullptr; }
        bool HasConstruct()     const { return Construct    != nullptr; }
        bool HasDestruct()      const { return Destruct     != nullptr; }
        #if USING(WITH_EDITOR)
        bool HasPreEdit()       const { return PreEdit      != nullptr; }
        bool HasPostEdit()      const { return PostEdit     != nullptr; }
        #endif
    };

    template<typename T>
    FStructOps* MakeStructOps()
    {
        FStructOps* Ops = new FStructOps{};

        if constexpr (eastl::is_default_constructible_v<T>)
        {
            Ops->Construct = +[](void* Mem)
            {
                new (Mem) T();
            };
        }

        if constexpr (eastl::is_destructible_v<T>)
        {
            Ops->Destruct = +[](void* Mem)
            {
                static_cast<T*>(Mem)->~T();
            };
        }

        if constexpr (Concepts::THasSerialize<T>)
        {
            Ops->Serialize = +[](FArchive& Ar, void* Value)
            {
                return static_cast<T*>(Value)->Serialize(Ar);
            };
        }

        if constexpr (Concepts::THasNetSerialize<T>)
        {
            Ops->NetSerialize = +[](FNetArchive& Ar, void* Value)
            {
                static_cast<T*>(Value)->NetSerialize(Ar);
            };
        }
        
        if constexpr (Concepts::THasCopy<T>)
        {
            Ops->Copy = +[](void* Dst, const void* Src)
            {
                static_cast<T*>(Dst)->CopyFrom(*static_cast<const T*>(Src));
            };
        }
        else if constexpr (eastl::is_copy_assignable_v<T>)
        {
            // Without this, a type with no CopyFrom leaves Ops->Copy null, and FStructProperty::
            // CopyCompleteValue silently falls through to walking the reflected property chain. That is a
            // no-op for any REFLECT(ManualStub) type -- every math type -- because a stub registers the
            // type and its ops but NO properties. The result was a copy that did nothing and reported
            // nothing: reset-to-default on a transform, prefab override inheritance of a vector leaf, and
            // multi-entity property replication all failed silently.
            Ops->Copy = +[](void* Dst, const void* Src)
            {
                *static_cast<T*>(Dst) = *static_cast<const T*>(Src);
            };
        }
        
        if constexpr (Concepts::THasEquality<T>)
        {
            Ops->Equals = +[](const void* LHS, const void* RHS)
            {
                return *static_cast<const T*>(LHS) == *static_cast<const T*>(RHS);
            };
        }
        
        if constexpr (Concepts::THasToString<T>)
        {
            Ops->ToString = +[](const void* Data)
            {
                return static_cast<const T*>(Data)->ToString();
            };
        }
        
        if constexpr (Concepts::THasLessThan<T>)
        {
            Ops->LessThan = +[](const void* LHS, const void* RHS)
            {
                return *static_cast<const T*>(LHS) < *static_cast<const T*>(RHS);
            };
        }
        
        #if USING(WITH_EDITOR)
        if constexpr (Concepts::THasPreEdit<T>)
        {
            Ops->PreEdit = +[](void* Data, const FPropertyChangedEvent& Event)
            {
                static_cast<T*>(Data)->PreEditChange(Event);
            };
        }
        
        if constexpr (Concepts::THasPostEdit<T>)
        {
            Ops->PostEdit = +[](void* Data, const FPropertyChangedEvent& Event)
            {
                static_cast<T*>(Data)->PostEditChange(Event);
            };
        }
        #endif
        return Ops;
    }
}
