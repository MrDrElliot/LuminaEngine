#pragma once

#include <concepts>
#include <initializer_list>
#include <type_traits>

#include "Containers/Pair.h"
#include "Containers/Span.h"
#include "Containers/String.h"
#include "Containers/StringView.h"
#include "Containers/Vector.h"
#include "Memory/Memcpy.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    enum class EJsonType : uint8
    {
        Null,
        Bool,
        Int,
        UInt,
        Double,
        String,
        Array,
        Object,
    };

    enum class EJsonParseFlags : uint8
    {
        None                = 0,
        AllowComments       = 1 << 0,
        AllowTrailingCommas = 1 << 1,
    };

    NODISCARD constexpr EJsonParseFlags operator|(EJsonParseFlags A, EJsonParseFlags B) noexcept
    {
        return static_cast<EJsonParseFlags>(static_cast<uint8>(A) | static_cast<uint8>(B));
    }

    NODISCARD constexpr bool HasJsonFlag(EJsonParseFlags Flags, EJsonParseFlags Flag) noexcept
    {
        return (static_cast<uint8>(Flags) & static_cast<uint8>(Flag)) != 0;
    }

    struct FJsonParseError
    {
        FString Message;
        int32   Line   = 0;
        int32   Column = 0;
        int64   Offset = 0;
    };

    // Up to 13 bytes live inline; longer text gets one heap block. Always null terminated.
    class RUNTIME_API FJsonString
    {
    public:

        static constexpr int32 kInlineCapacity = 13;

        FJsonString() noexcept { Data[0] = 0; Data[kTagIndex] = 0; }
        explicit FJsonString(FStringView Text);
        FJsonString(const FJsonString& Other);
        FJsonString(FJsonString&& Other) noexcept;
        ~FJsonString() { Release(); }

        FJsonString& operator=(const FJsonString& Other);
        FJsonString& operator=(FJsonString&& Other) noexcept;

        NODISCARD FORCEINLINE int32 Len() const noexcept
        {
            if (IsHeap())
            {
                int32 Length;
                Memory::Memcpy(&Length, Data + 8, sizeof(Length));
                return Length;
            }
            return static_cast<uint8>(Data[kTagIndex]);
        }

        NODISCARD FORCEINLINE const char* CStr() const noexcept
        {
            if (IsHeap())
            {
                const char* Heap;
                Memory::Memcpy(&Heap, Data, sizeof(Heap));
                return Heap;
            }
            return Data;
        }

        NODISCARD FORCEINLINE FStringView View() const noexcept { return FStringView(CStr(), Len()); }
        NODISCARD FORCEINLINE bool IsEmpty() const noexcept { return Len() == 0; }
        NODISCARD FORCEINLINE operator FStringView() const noexcept { return View(); }
        NODISCARD FString ToString() const { return FString(CStr(), Len()); }

        NODISCARD friend bool operator==(const FJsonString& A, const FJsonString& B) noexcept { return A.View() == B.View(); }
        NODISCARD friend bool operator==(const FJsonString& A, FStringView B) noexcept { return A.View() == B; }
        NODISCARD friend bool operator==(const FJsonString& A, const char* B) noexcept { return A.View() == FStringView(B); }

    private:

        friend class FJsonValue;
        friend class FJsonParser;

        static constexpr int32 kTagIndex   = 14;
        static constexpr uint8 kHeapMarker = 0xFF;

        NODISCARD FORCEINLINE bool IsHeap() const noexcept { return static_cast<uint8>(Data[kTagIndex]) == kHeapMarker; }

        void Assign(const char* Text, int32 Length);
        void Release() noexcept;

        char Data[15];
    };

    class FJsonValue;

    struct FJsonMember;

    // A JSON document node. Building goes through Set, FindOrAdd and Add; operator[] only reads and yields null for anything missing.
    class alignas(8) RUNTIME_API FJsonValue
    {
    public:

        FJsonValue() noexcept { Type = EJsonType::Null; }
        FJsonValue(std::nullptr_t) noexcept : FJsonValue() {}
        FJsonValue(bool Value) noexcept { SetPayload(Value); Type = EJsonType::Bool; }

        template <std::integral T>
        requires (!std::same_as<T, bool>)
        FJsonValue(T Value) noexcept
        {
            if constexpr (std::is_signed_v<T>)
            {
                SetPayload(static_cast<int64>(Value));
                Type = EJsonType::Int;
            }
            else
            {
                SetPayload(static_cast<uint64>(Value));
                Type = EJsonType::UInt;
            }
        }

        template <std::floating_point T>
        FJsonValue(T Value) noexcept { SetPayload(static_cast<double>(Value)); Type = EJsonType::Double; }

        FJsonValue(const char* Text) : FJsonValue(FStringView(Text)) {}
        FJsonValue(FStringView Text);
        FJsonValue(const FString& Text) : FJsonValue(Text.View()) {}
        FJsonValue(const FJsonString& Text);

        // Without this, any other pointer would quietly become a bool.
        template <typename T>
        FJsonValue(T*) = delete;

        FJsonValue(const FJsonValue& Other);
        FJsonValue(FJsonValue&& Other) noexcept;
        ~FJsonValue() { Release(); }

        FJsonValue& operator=(const FJsonValue& Other);
        FJsonValue& operator=(FJsonValue&& Other) noexcept;

        NODISCARD static FJsonValue MakeArray(int32 Reserve = 0);
        NODISCARD static FJsonValue MakeArray(std::initializer_list<FJsonValue> Items);
        NODISCARD static FJsonValue MakeObject(int32 Reserve = 0);
        NODISCARD static FJsonValue MakeObject(std::initializer_list<TPair<FStringView, FJsonValue>> Members);

        NODISCARD static bool Parse(FStringView Text, FJsonValue& Out, FJsonParseError* OutError = nullptr, EJsonParseFlags Flags = EJsonParseFlags::None);

        void WriteTo(FString& Out, int32 Indent = -1) const;
        NODISCARD FString ToString(int32 Indent = -1) const;

        NODISCARD FORCEINLINE EJsonType GetType() const noexcept { return Type; }
        NODISCARD FORCEINLINE bool IsNull() const noexcept { return Type == EJsonType::Null; }
        NODISCARD FORCEINLINE bool IsBool() const noexcept { return Type == EJsonType::Bool; }
        NODISCARD FORCEINLINE bool IsNumber() const noexcept { return Type == EJsonType::Int || Type == EJsonType::UInt || Type == EJsonType::Double; }
        NODISCARD FORCEINLINE bool IsInteger() const noexcept { return Type == EJsonType::Int || Type == EJsonType::UInt; }
        NODISCARD FORCEINLINE bool IsUnsigned() const noexcept { return Type == EJsonType::UInt; }
        NODISCARD FORCEINLINE bool IsDouble() const noexcept { return Type == EJsonType::Double; }
        NODISCARD FORCEINLINE bool IsString() const noexcept { return Type == EJsonType::String; }
        NODISCARD FORCEINLINE bool IsArray() const noexcept { return Type == EJsonType::Array; }
        NODISCARD FORCEINLINE bool IsObject() const noexcept { return Type == EJsonType::Object; }

        // Each As accessor returns Default unless the value holds that kind; numbers convert between each other as a cast would.
        NODISCARD bool AsBool(bool Default = false) const noexcept { return IsBool() ? GetPayload<bool>() : Default; }
        NODISCARD int64 AsInt64(int64 Default = 0) const noexcept;
        NODISCARD uint64 AsUInt64(uint64 Default = 0) const noexcept;
        NODISCARD double AsDouble(double Default = 0.0) const noexcept;
        NODISCARD int32 AsInt32(int32 Default = 0) const noexcept { return IsNumber() ? static_cast<int32>(AsInt64()) : Default; }
        NODISCARD uint32 AsUInt32(uint32 Default = 0) const noexcept { return IsNumber() ? static_cast<uint32>(AsUInt64()) : Default; }
        NODISCARD float AsFloat(float Default = 0.0f) const noexcept { return IsNumber() ? static_cast<float>(AsDouble()) : Default; }
        NODISCARD FStringView AsString(FStringView Default = {}) const noexcept { return IsString() ? StringPayload().View() : Default; }
        NODISCARD const char* AsCString(const char* Default = "") const noexcept { return IsString() ? StringPayload().CStr() : Default; }

        bool TryGet(bool& Out) const noexcept;
        bool TryGet(int32& Out) const noexcept;
        bool TryGet(int64& Out) const noexcept;
        bool TryGet(uint32& Out) const noexcept;
        bool TryGet(uint64& Out) const noexcept;
        bool TryGet(float& Out) const noexcept;
        bool TryGet(double& Out) const noexcept;
        bool TryGet(FStringView& Out) const noexcept;
        bool TryGet(FString& Out) const;

        // The member's value when it exists and holds T, otherwise Default; it never throws.
        template <typename T>
        NODISCARD T GetValue(FStringView Key, T Default) const
        {
            if (const FJsonValue* Member = Find(Key))
            {
                T Out;
                if (Member->TryGet(Out))
                {
                    return Out;
                }
            }
            return Default;
        }

        NODISCARD FString GetString(FStringView Key, FStringView Default = {}) const;

        // Elements for an array, members for an object, zero for anything else.
        NODISCARD int32 Num() const noexcept;
        NODISCARD bool IsEmpty() const noexcept { return Num() == 0; }

        NODISCARD TSpan<const FJsonValue> AsArray() const noexcept;
        NODISCARD TSpan<FJsonValue> AsArray() noexcept;

        // Changing a member's Key in place breaks lookups; Remove and Set it instead.
        NODISCARD TSpan<const FJsonMember> Members() const noexcept;
        NODISCARD TSpan<FJsonMember> Members() noexcept;

        NODISCARD const FJsonValue& operator[](int32 Index) const noexcept;
        NODISCARD const FJsonValue& operator[](FStringView Key) const noexcept;
        NODISCARD const FJsonValue& operator[](const char* Key) const noexcept { return (*this)[FStringView(Key)]; }

        NODISCARD const FJsonValue* Find(FStringView Key) const noexcept;
        NODISCARD FJsonValue* Find(FStringView Key) noexcept;
        NODISCARD bool Contains(FStringView Key) const noexcept { return Find(Key) != nullptr; }

        // These turn a null value into an empty array or object first, so a document can be built from nothing.
        FJsonValue& Add(FJsonValue Value);
        FJsonValue& Set(FStringView Key, FJsonValue Value);
        FJsonValue& FindOrAdd(FStringView Key);

        bool Remove(FStringView Key);
        void RemoveAt(int32 Index);
        void Reserve(int32 Count);

        // RFC 7386: null members of Patch delete, objects merge recursively, everything else replaces.
        void MergePatch(const FJsonValue& Patch);

        void Reset() noexcept { Release(); Type = EJsonType::Null; }

        NODISCARD friend bool operator==(const FJsonValue& A, const FJsonValue& B) noexcept { return A.Equals(B); }

    private:

        friend class FJsonParser;
        friend class FJsonWriter;

        template <typename T>
        FORCEINLINE void SetPayload(T Value) noexcept
        {
            static_assert(sizeof(T) <= 8);
            Memory::Memcpy(Storage, &Value, sizeof(T));
        }

        template <typename T>
        NODISCARD FORCEINLINE T GetPayload() const noexcept
        {
            T Value;
            Memory::Memcpy(&Value, Storage, sizeof(T));
            return Value;
        }

        NODISCARD FORCEINLINE FJsonString& StringPayload() noexcept { return *reinterpret_cast<FJsonString*>(Storage); }
        NODISCARD FORCEINLINE const FJsonString& StringPayload() const noexcept { return *reinterpret_cast<const FJsonString*>(Storage); }

        NODISCARD FORCEINLINE void* PointerPayload() const noexcept { return GetPayload<void*>(); }

        void EnsureArray();
        void EnsureObject();
        void CopyFrom(const FJsonValue& Other);
        void Release() noexcept;
        NODISCARD bool Equals(const FJsonValue& Other) const noexcept;

        uint8     Storage[15];
        EJsonType Type;
    };

    struct FJsonMember
    {
        FJsonString Key;
        FJsonValue  Value;
    };

    static_assert(sizeof(FJsonString) == 15);
    static_assert(sizeof(FJsonValue) == 16);

    template <>
    struct TIsTriviallyRelocatable<FJsonString>
    {
        static constexpr bool Value = true;
    };

    template <>
    struct TIsTriviallyRelocatable<FJsonValue>
    {
        static constexpr bool Value = true;
    };

    template <>
    struct TIsTriviallyRelocatable<FJsonMember>
    {
        static constexpr bool Value = true;
    };
}
