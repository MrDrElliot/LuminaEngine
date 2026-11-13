#pragma once

#include <type_traits>

#include "Containers/Algorithm.h"
#include "Containers/Name.h"
#include "Containers/Span.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Variant/Variant.h"

namespace Lumina::Lua
{
    // What a Lua stack slot holds, with light userdata standing in for the object a binding runs on.
    using FValue = TVariant<bool, int64, double, FString, void*>;

    // A lua_CFunction's stack, arguments from index zero and results pushed after them.
    using FStack = TVector<FValue>;

    // Returns how many results it pushed, as a lua_CFunction does.
    using FThunk = int32 (*)(FStack& Stack);

    // One luaL_Reg row.
    struct FReg
    {
        FStringView Name;
        FThunk      Thunk;
    };

    // A class's metatable, every table sorted by name so a lookup is a binary search.
    struct FClassBinding
    {
        FStringView       Name;
        TSpan<const FReg> Getters;
        TSpan<const FReg> Setters;
        TSpan<const FReg> Methods;
    };

    // Every class the generator bound in this module.
    REFLECTIONEXAMPLERUNTIME_API TSpan<const FClassBinding> GetClassBindings();

    template<typename TEntry>
    const TEntry* Find(TSpan<const TEntry> Table, FStringView Name)
    {
        const TEntry* Found = Algo::LowerBound(Table.begin(), Table.end(), Name, [](const TEntry& Entry, FStringView Key) { return Entry.Name < Key; });
        return Found != Table.end() && Found->Name == Name ? Found : nullptr;
    }

    template<typename T>
    T* Self(const FStack& Stack)
    {
        return static_cast<T*>(Stack[0].Get<void*>());
    }

    // Reads slot Index as T, converting between Lua's integer and float numbers as Lua does.
    template<typename T>
    T Check(const FStack& Stack, int32 Index)
    {
        const FValue& Value = Stack[Index];
        if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, FString>)
        {
            return Value.Get<T>();
        }
        else if constexpr (std::is_same_v<T, FName>)
        {
            return FName(Value.Get<FString>());
        }
        else
        {
            return Value.IsType<int64>() ? static_cast<T>(Value.Get<int64>()) : static_cast<T>(Value.Get<double>());
        }
    }

    template<typename T>
    void Push(FStack& Stack, const T& Value)
    {
        if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, FString>)
        {
            Stack.Add(FValue(Value));
        }
        else if constexpr (std::is_same_v<T, FName>)
        {
            Stack.Add(FValue(Value.ToString()));
        }
        else if constexpr (std::is_integral_v<T>)
        {
            Stack.Add(FValue(static_cast<int64>(Value)));
        }
        else
        {
            Stack.Add(FValue(static_cast<double>(Value)));
        }
    }
}
