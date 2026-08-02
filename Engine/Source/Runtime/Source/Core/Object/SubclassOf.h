#pragma once
#include "Class.h"

namespace Lumina
{
    // Reflectable handle to a CClass guaranteed to be T or a subclass of T. Lets a property pick a
    // class TYPE (not an instance) at runtime; the editor renders a type dropdown and serialization
    // persists the class by name.
    //
    // Layout invariant: exactly one CClass* (pointer-sized) at offset 0. FClassProperty reads the
    // member as a CClass* directly, so don't add members.
    template<typename T>
    class TSubclassOf
    {
    public:

        TSubclassOf() = default;
        TSubclassOf(decltype(nullptr)) {}
        TSubclassOf(CClass* InClass) { Set(InClass); }

        TSubclassOf& operator=(decltype(nullptr)) { Class = nullptr; return *this; }
        TSubclassOf& operator=(CClass* InClass) { Set(InClass); return *this; }

        CClass* Get() const { return Class; }
        operator CClass*() const { return Class; }
        CClass* operator->() const { return Class; }

        bool IsValid() const { return Class != nullptr; }
        explicit operator bool() const { return Class != nullptr; }

        bool operator==(const TSubclassOf& Other) const { return Class == Other.Class; }
        bool operator!=(const TSubclassOf& Other) const { return Class != Other.Class; }

        void Reset() { Class = nullptr; }

        // The class default object cast to T (null if unset).
        T* GetDefaultObject() const { return Class ? Class->template GetDefaultObject<T>() : nullptr; }

        // The compile-time base class every assignable value must derive from.
        static CClass* StaticBaseClass() { return T::StaticClass(); }

    private:

        void Set(CClass* InClass)
        {
            // Reject a class that isn't actually T or derived, mirroring TSubclassOf's contract.
            Class = (InClass && InClass->IsChildOf(T::StaticClass())) ? InClass : nullptr;
        }

        CClass* Class = nullptr;
    };

    // Reflectable handle to a CStruct guaranteed to be T or a struct derived from T.
    template<typename T>
    class TSubStructOf
    {
    public:

        TSubStructOf() = default;
        TSubStructOf(decltype(nullptr)) {}
        TSubStructOf(CStruct* InStruct) { Set(InStruct); }

        TSubStructOf& operator=(decltype(nullptr)) { Struct = nullptr; return *this; }
        TSubStructOf& operator=(CStruct* InStruct) { Set(InStruct); return *this; }

        CStruct* Get() const { return Struct; }
        operator CStruct*() const { return Struct; }
        CStruct* operator->() const { return Struct; }

        bool IsValid() const { return Struct != nullptr; }
        explicit operator bool() const { return Struct != nullptr; }

        bool operator==(const TSubStructOf& Other) const { return Struct == Other.Struct; }
        bool operator!=(const TSubStructOf& Other) const { return Struct != Other.Struct; }

        void Reset() { Struct = nullptr; }

        // The compile-time base struct every assignable value must derive from.
        static CStruct* StaticBaseStruct() { return T::StaticStruct(); }

    private:

        void Set(CStruct* InStruct)
        {
            Struct = (InStruct && InStruct->IsChildOf(T::StaticStruct())) ? InStruct : nullptr;
        }

        CStruct* Struct = nullptr;
    };
}
