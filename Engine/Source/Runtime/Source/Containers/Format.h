#pragma once

#include <type_traits>
#include <utility>

#include "ContainerAllocator.h"
#include "ContainerTraits.h"
#include "StringView.h"
#include "Memory/Memcpy.h"

namespace Lumina::Fmt
{
    enum class EFormatAlign : uint8
    {
        Default,
        Left,
        Right,
        Center,
    };

    enum class EFormatSign : uint8
    {
        Minus,
        Plus,
        Space,
    };

    /** One replacement field's specifier, already parsed. Width and Precision are -1 when absent. */
    struct FFormatSpec
    {
        int32        Width      = -1;
        int32        Precision  = -1;
        char         Fill       = ' ';
        char         Type       = '\0';
        EFormatAlign Align      = EFormatAlign::Default;
        EFormatSign  Sign       = EFormatSign::Minus;
        bool         bAlternate = false;
        bool         bZeroPad   = false;
    };

    /** Growable character sink; the data pointer moves on writes, so never hold one across a write. */
    class FFormatBuffer
    {
    public:

        using FGrow = void (*)(FFormatBuffer&, size_t);

        FFormatBuffer(char* InData, size_t InCapacity, FGrow InGrow) noexcept
            : Ptr(InData), Cap(InCapacity), GrowFn(InGrow)
        {
        }

        FFormatBuffer(const FFormatBuffer&) = delete;
        FFormatBuffer& operator=(const FFormatBuffer&) = delete;

        NODISCARD FORCEINLINE char* Data() noexcept { return Ptr; }
        NODISCARD FORCEINLINE const char* Data() const noexcept { return Ptr; }
        NODISCARD FORCEINLINE size_t Size() const noexcept { return Used; }
        NODISCARD FORCEINLINE size_t Capacity() const noexcept { return Cap; }
        NODISCARD FORCEINLINE bool Empty() const noexcept { return Used == 0; }
        NODISCARD FORCEINLINE FStringView View() const noexcept { return FStringView(Ptr, Used); }

        FORCEINLINE void Clear() noexcept { Used = 0; }

        FORCEINLINE void Push(char Character)
        {
            if (Used == Cap)
            {
                GrowFn(*this, Used + 1);
                if (Used == Cap)
                {
                    return;
                }
            }

            Ptr[Used++] = Character;
        }

        FORCEINLINE void Append(const char* Data, size_t Count)
        {
            if (Count == 0)
            {
                return;
            }

            if (Used + Count > Cap)
            {
                GrowFn(*this, Used + Count);

                // A fixed buffer cannot grow, so it takes the prefix that fits and reports truncation.
                if (Used + Count > Cap)
                {
                    Count = Cap > Used ? Cap - Used : 0;
                    if (Count == 0)
                    {
                        return;
                    }
                }
            }

            Memory::Memcpy(Ptr + Used, Data, Count);
            Used += Count;
        }

        FORCEINLINE void Append(FStringView Text) { Append(Text.data(), Text.size()); }

        void AppendFill(char Character, size_t Count)
        {
            if (Count == 0)
            {
                return;
            }

            if (Used + Count > Cap)
            {
                GrowFn(*this, Used + Count);

                if (Used + Count > Cap)
                {
                    Count = Cap > Used ? Cap - Used : 0;
                    if (Count == 0)
                    {
                        return;
                    }
                }
            }

            char* Write = Ptr + Used;
            for (size_t Index = 0; Index < Count; ++Index)
            {
                Write[Index] = Character;
            }

            Used += Count;
        }

        /** Hands back a writable run of Count bytes; follow with Advance once the real length is known. */
        FORCEINLINE char* ReserveTail(size_t Count)
        {
            if (Used + Count > Cap)
            {
                GrowFn(*this, Used + Count);
                LUMINA_CONTAINER_CHECK(Used + Count <= Cap);
            }

            return Ptr + Used;
        }

        FORCEINLINE void Advance(size_t Count) noexcept { Used += Count; }

    protected:

        char*  Ptr;
        size_t Used = 0;
        size_t Cap;
        FGrow  GrowFn;
    };

    /** Formats into a stack buffer and only reaches the allocator when the text outgrows it. */
    template <size_t InlineCapacity = 256, ContainerAllocatorType TAllocator = FHeapAllocator>
    class TInlineFormatBuffer final : public FFormatBuffer
    {
    public:

        TInlineFormatBuffer() noexcept : FFormatBuffer(Storage, InlineCapacity, &GrowImpl) {}

        ~TInlineFormatBuffer()
        {
            if (Ptr != Storage)
            {
                TAllocator::Deallocate(Ptr, Cap, alignof(char));
            }
        }

    private:

        static void GrowImpl(FFormatBuffer& Base, size_t Required)
        {
            TInlineFormatBuffer& Self = static_cast<TInlineFormatBuffer&>(Base);

            size_t NewCapacity = Self.Cap * 2;
            if (NewCapacity < Required)
            {
                NewCapacity = Required;
            }

            char* Block = static_cast<char*>(TAllocator::Allocate(NewCapacity, alignof(char)));
            Memory::Memcpy(Block, Self.Ptr, Self.Used);

            if (Self.Ptr != Self.Storage)
            {
                TAllocator::Deallocate(Self.Ptr, Self.Cap, alignof(char));
            }

            Self.Ptr = Block;
            Self.Cap = NewCapacity;
        }

        char Storage[InlineCapacity];
    };

    /** Writes into a caller-owned array and drops whatever does not fit. */
    class FFixedFormatBuffer final : public FFormatBuffer
    {
    public:

        FFixedFormatBuffer(char* InData, size_t InCapacity) noexcept
            : FFormatBuffer(InData, InCapacity, &GrowImpl)
        {
        }

        NODISCARD bool Truncated() const noexcept { return bTruncated; }

    private:

        static void GrowImpl(FFormatBuffer& Base, size_t)
        {
            static_cast<FFixedFormatBuffer&>(Base).bTruncated = true;
        }

        bool bTruncated = false;
    };

    enum class EFormatArgType : uint8
    {
        None,
        Bool,
        Char,
        Int64,
        UInt64,
        Float,
        Double,
        CString,
        String,
        Pointer,
        Custom,
    };

    using FFormatThunk = void (*)(FFormatBuffer&, const void*, const FFormatSpec&);

    struct FFormatArg
    {
        union
        {
            bool        BoolValue;
            char        CharValue;
            int64       Int64Value;
            uint64      UInt64Value;
            float       FloatValue;
            double      DoubleValue;
            const char* CStringValue;
            const void* PointerValue;

            struct
            {
                const char* Data;
                size_t      Size;
            } StringValue;

            struct
            {
                const void*  Value;
                FFormatThunk Thunk;
            } CustomValue;
        };

        EFormatArgType Type = EFormatArgType::None;

        constexpr FFormatArg() noexcept : Int64Value(0) {}
    };

    struct FFormatArgs
    {
        const FFormatArg* Values = nullptr;
        uint32            Count  = 0;
    };

    /** Anything that is not arithmetic, a pointer or string-like reaches formatting through this. */
    template <typename T>
    concept CustomFormattable = requires(FFormatBuffer& Out, const T& Value, const FFormatSpec& Spec)
    {
        FormatArgument(Out, Value, Spec);
    };

    template <typename T>
    concept StringFormattable = std::convertible_to<const T&, FStringView>;

    template <typename T>
    concept CharRangeFormattable = !StringFormattable<T> && requires(const T& Value)
    {
        { Value.data() } -> std::convertible_to<const char*>;
        { Value.size() } -> std::convertible_to<size_t>;
    };

    template <typename T>
    void FormatCustomThunk(FFormatBuffer& Out, const void* Value, const FFormatSpec& Spec)
    {
        FormatArgument(Out, *static_cast<const T*>(Value), Spec);
    }

    template <typename T>
    NODISCARD FFormatArg MakeFormatArg(const T& Value)
    {
        using FValue = std::remove_cvref_t<T>;

        FFormatArg Arg;

        if constexpr (std::is_same_v<FValue, bool>)
        {
            Arg.BoolValue = Value;
            Arg.Type = EFormatArgType::Bool;
        }
        else if constexpr (std::is_same_v<FValue, char>)
        {
            Arg.CharValue = Value;
            Arg.Type = EFormatArgType::Char;
        }
        else if constexpr (std::is_enum_v<FValue> && !CustomFormattable<FValue>)
        {
            Arg.Int64Value = static_cast<int64>(static_cast<std::underlying_type_t<FValue>>(Value));
            Arg.Type = EFormatArgType::Int64;
        }
        else if constexpr (std::is_integral_v<FValue> && std::is_signed_v<FValue>)
        {
            Arg.Int64Value = static_cast<int64>(Value);
            Arg.Type = EFormatArgType::Int64;
        }
        else if constexpr (std::is_integral_v<FValue>)
        {
            Arg.UInt64Value = static_cast<uint64>(Value);
            Arg.Type = EFormatArgType::UInt64;
        }
        else if constexpr (std::is_same_v<FValue, float>)
        {
            Arg.FloatValue = Value;
            Arg.Type = EFormatArgType::Float;
        }
        else if constexpr (std::is_floating_point_v<FValue>)
        {
            Arg.DoubleValue = static_cast<double>(Value);
            Arg.Type = EFormatArgType::Double;
        }
        else if constexpr (std::is_same_v<FValue, const char*> || std::is_same_v<FValue, char*>)
        {
            Arg.CStringValue = Value;
            Arg.Type = EFormatArgType::CString;
        }
        else if constexpr (StringFormattable<FValue>)
        {
            const FStringView Text = static_cast<FStringView>(Value);
            Arg.StringValue.Data = Text.data();
            Arg.StringValue.Size = Text.size();
            Arg.Type = EFormatArgType::String;
        }
        else if constexpr (CharRangeFormattable<FValue> && !CustomFormattable<FValue>)
        {
            Arg.StringValue.Data = Value.data();
            Arg.StringValue.Size = Value.size();
            Arg.Type = EFormatArgType::String;
        }
        else if constexpr (CustomFormattable<FValue>)
        {
            Arg.CustomValue.Value = static_cast<const void*>(std::addressof(Value));
            Arg.CustomValue.Thunk = &FormatCustomThunk<FValue>;
            Arg.Type = EFormatArgType::Custom;
        }
        else if constexpr (std::is_pointer_v<FValue>)
        {
            Arg.PointerValue = static_cast<const void*>(Value);
            Arg.Type = EFormatArgType::Pointer;
        }
        else
        {
            static_assert(sizeof(FValue) == 0,
                "This type cannot be formatted. Declare FormatArgument(FFormatBuffer&, const T&, "
                "const FFormatSpec&) next to it so ADL finds it.");
        }

        return Arg;
    }

    template <typename T>
    NODISCARD consteval EFormatArgType FormatArgTypeOf()
    {
        using FValue = std::remove_cvref_t<T>;

        if constexpr (std::is_same_v<FValue, bool>)                                { return EFormatArgType::Bool; }
        else if constexpr (std::is_same_v<FValue, char>)                           { return EFormatArgType::Char; }
        else if constexpr (std::is_enum_v<FValue> && !CustomFormattable<FValue>)   { return EFormatArgType::Int64; }
        else if constexpr (std::is_integral_v<FValue> && std::is_signed_v<FValue>) { return EFormatArgType::Int64; }
        else if constexpr (std::is_integral_v<FValue>)                             { return EFormatArgType::UInt64; }
        else if constexpr (std::is_same_v<FValue, float>)                          { return EFormatArgType::Float; }
        else if constexpr (std::is_floating_point_v<FValue>)                       { return EFormatArgType::Double; }
        else if constexpr (std::is_same_v<FValue, const char*> ||
                           std::is_same_v<FValue, char*>)                          { return EFormatArgType::CString; }
        else if constexpr (StringFormattable<FValue>)                              { return EFormatArgType::String; }
        else if constexpr (CharRangeFormattable<FValue> && !CustomFormattable<FValue>)
                                                                                   { return EFormatArgType::String; }
        else if constexpr (CustomFormattable<FValue>)                              { return EFormatArgType::Custom; }
        else if constexpr (std::is_pointer_v<FValue>)                              { return EFormatArgType::Pointer; }
        else                                                                       { return EFormatArgType::None; }
    }

    template <typename... TArgs>
    struct TFormatArgStore
    {
        static constexpr size_t kCount = sizeof...(TArgs);

        FFormatArg Values[kCount == 0 ? 1 : kCount];

        explicit TFormatArgStore(const TArgs&... Args) : Values{ MakeFormatArg(Args)... } {}

        NODISCARD FFormatArgs View() const noexcept
        {
            return FFormatArgs{ Values, static_cast<uint32>(kCount) };
        }
    };

    template <>
    struct TFormatArgStore<>
    {
        static constexpr size_t kCount = 0;

        NODISCARD FFormatArgs View() const noexcept { return FFormatArgs{}; }
    };

    namespace Private
    {
        // Deliberately not constexpr: calling one inside the consteval check names the fault and stops the build.
        void FormatStringError_UnmatchedOpeningBrace();
        void FormatStringError_UnmatchedClosingBrace();
        void FormatStringError_ArgumentIndexOutOfRange();
        void FormatStringError_MixedAutomaticAndManualIndexing();
        void FormatStringError_InvalidSpecifier();
        void FormatStringError_SpecifierRejectedByArgumentType();

        NODISCARD constexpr bool AcceptsTypeChar(EFormatArgType ArgType, char TypeChar) noexcept
        {
            if (TypeChar == '\0' || ArgType == EFormatArgType::Custom || ArgType == EFormatArgType::None)
            {
                return true;
            }

            switch (ArgType)
            {
            case EFormatArgType::Bool:
                return TypeChar == 's' || TypeChar == 'b' || TypeChar == 'B' || TypeChar == 'd' ||
                       TypeChar == 'o' || TypeChar == 'x' || TypeChar == 'X';
            case EFormatArgType::Char:
            case EFormatArgType::Int64:
            case EFormatArgType::UInt64:
                return TypeChar == 'c' || TypeChar == 'b' || TypeChar == 'B' || TypeChar == 'd' ||
                       TypeChar == 'o' || TypeChar == 'x' || TypeChar == 'X';
            case EFormatArgType::Float:
            case EFormatArgType::Double:
                return TypeChar == 'a' || TypeChar == 'A' || TypeChar == 'e' || TypeChar == 'E' ||
                       TypeChar == 'f' || TypeChar == 'F' || TypeChar == 'g' || TypeChar == 'G';
            case EFormatArgType::CString:
            case EFormatArgType::String:
                return TypeChar == 's';
            case EFormatArgType::Pointer:
                return TypeChar == 'p' || TypeChar == 'P';
            default:
                return true;
            }
        }

        NODISCARD constexpr bool IsDigit(char Character) noexcept
        {
            return Character >= '0' && Character <= '9';
        }

        /** Walks the format string the way the runtime parser does, but only to reject bad input. */
        constexpr void ValidateFormatString(FStringView Fmt, const EFormatArgType* Types, size_t TypeCount)
        {
            size_t Index = 0;
            uint32 NextAutomatic = 0;
            bool   bUsedAutomatic = false;
            bool   bUsedManual = false;

            const auto ReadIndex = [&](uint32& OutIndex) constexpr
            {
                if (Index < Fmt.size() && IsDigit(Fmt[Index]))
                {
                    uint32 Value = 0;
                    while (Index < Fmt.size() && IsDigit(Fmt[Index]))
                    {
                        Value = Value * 10 + static_cast<uint32>(Fmt[Index] - '0');
                        ++Index;
                    }

                    bUsedManual = true;
                    OutIndex = Value;
                }
                else
                {
                    bUsedAutomatic = true;
                    OutIndex = NextAutomatic++;
                }

                if (bUsedAutomatic && bUsedManual)
                {
                    FormatStringError_MixedAutomaticAndManualIndexing();
                }
            };

            // Checked only once the field is known to close, so a stray brace reports as a stray brace.
            uint32 HighestIndex = 0;

            while (Index < Fmt.size())
            {
                const char Character = Fmt[Index];

                if (Character == '}')
                {
                    if (Index + 1 < Fmt.size() && Fmt[Index + 1] == '}')
                    {
                        Index += 2;
                        continue;
                    }

                    FormatStringError_UnmatchedClosingBrace();
                    return;
                }

                if (Character != '{')
                {
                    ++Index;
                    continue;
                }

                if (Index + 1 < Fmt.size() && Fmt[Index + 1] == '{')
                {
                    Index += 2;
                    continue;
                }

                ++Index;

                uint32 ArgIndex = 0;
                ReadIndex(ArgIndex);
                HighestIndex = ArgIndex;

                const EFormatArgType ArgType =
                    ArgIndex < TypeCount ? Types[ArgIndex] : EFormatArgType::None;

                if (Index < Fmt.size() && Fmt[Index] == ':')
                {
                    ++Index;

                    // Fill only counts as fill when an alignment character follows it.
                    if (Index + 1 < Fmt.size() &&
                        (Fmt[Index + 1] == '<' || Fmt[Index + 1] == '>' || Fmt[Index + 1] == '^'))
                    {
                        Index += 2;
                    }
                    else if (Index < Fmt.size() &&
                             (Fmt[Index] == '<' || Fmt[Index] == '>' || Fmt[Index] == '^'))
                    {
                        ++Index;
                    }

                    if (Index < Fmt.size() && (Fmt[Index] == '+' || Fmt[Index] == '-' || Fmt[Index] == ' '))
                    {
                        ++Index;
                    }

                    if (Index < Fmt.size() && Fmt[Index] == '#')
                    {
                        ++Index;
                    }

                    if (Index < Fmt.size() && Fmt[Index] == '0')
                    {
                        ++Index;
                    }

                    if (Index < Fmt.size() && Fmt[Index] == '{')
                    {
                        ++Index;
                        uint32 WidthIndex = 0;
                        ReadIndex(WidthIndex);
                        HighestIndex = WidthIndex > HighestIndex ? WidthIndex : HighestIndex;
                        if (Index >= Fmt.size() || Fmt[Index] != '}')
                        {
                            FormatStringError_InvalidSpecifier();
                            return;
                        }
                        ++Index;
                    }
                    else
                    {
                        while (Index < Fmt.size() && IsDigit(Fmt[Index]))
                        {
                            ++Index;
                        }
                    }

                    if (Index < Fmt.size() && Fmt[Index] == '.')
                    {
                        ++Index;
                        if (Index < Fmt.size() && Fmt[Index] == '{')
                        {
                            ++Index;
                            uint32 PrecisionIndex = 0;
                            ReadIndex(PrecisionIndex);
                            HighestIndex = PrecisionIndex > HighestIndex ? PrecisionIndex : HighestIndex;
                            if (Index >= Fmt.size() || Fmt[Index] != '}')
                            {
                                FormatStringError_InvalidSpecifier();
                                return;
                            }
                            ++Index;
                        }
                        else if (Index < Fmt.size() && IsDigit(Fmt[Index]))
                        {
                            while (Index < Fmt.size() && IsDigit(Fmt[Index]))
                            {
                                ++Index;
                            }
                        }
                        else
                        {
                            FormatStringError_InvalidSpecifier();
                            return;
                        }
                    }

                    if (Index < Fmt.size() && Fmt[Index] == 'L')
                    {
                        ++Index;
                    }

                    if (Index < Fmt.size() && Fmt[Index] != '}')
                    {
                        const char TypeChar = Fmt[Index];
                        if (!AcceptsTypeChar(ArgType, TypeChar))
                        {
                            FormatStringError_SpecifierRejectedByArgumentType();
                            return;
                        }
                        ++Index;
                    }
                }

                if (Index >= Fmt.size() || Fmt[Index] != '}')
                {
                    FormatStringError_UnmatchedOpeningBrace();
                    return;
                }

                if (HighestIndex >= TypeCount)
                {
                    FormatStringError_ArgumentIndexOutOfRange();
                    return;
                }

                ++Index;
            }
        }
    }

    /** A format string checked where it is written, so a bad one is a compile error, not a bad log line. */
    template <typename... TArgs>
    class TFormatString
    {
    public:

        template <typename T>
        requires std::convertible_to<const T&, FStringView>
        consteval TFormatString(const T& Text) : Value(Text)
        {
            constexpr size_t kCount = sizeof...(TArgs);
            const EFormatArgType Types[kCount == 0 ? 1 : kCount] =
            {
                FormatArgTypeOf<TArgs>()...
            };

            Private::ValidateFormatString(Value, Types, kCount);
        }

        NODISCARD constexpr FStringView Get() const noexcept { return Value; }

    private:

        FStringView Value;
    };

    /** Opts a format string out of the compile-time check, for text that only exists at runtime. */
    struct FRuntimeFormatString
    {
        FStringView Value;

        explicit constexpr FRuntimeFormatString(FStringView InValue) noexcept : Value(InValue) {}
    };

    NODISCARD FORCEINLINE constexpr FRuntimeFormatString RuntimeFormat(FStringView Text) noexcept
    {
        return FRuntimeFormatString(Text);
    }

    /** The parser and every value writer sit behind this one symbol, compiled once for the whole engine. */
    RUNTIME_API void VFormatTo(FFormatBuffer& Out, FStringView Fmt, FFormatArgs Args);

    RUNTIME_API void WriteAligned(FFormatBuffer& Out, FStringView Text, const FFormatSpec& Spec);
    RUNTIME_API void WriteInteger(FFormatBuffer& Out, uint64 Magnitude, bool bNegative, const FFormatSpec& Spec);
    RUNTIME_API void WriteFloat(FFormatBuffer& Out, double Value, const FFormatSpec& Spec);
    RUNTIME_API void WriteFloat(FFormatBuffer& Out, float Value, const FFormatSpec& Spec);
}

namespace Lumina
{
    using Fmt::FFormatBuffer;
    using Fmt::FFormatSpec;
    using Fmt::RuntimeFormat;

    template <size_t InlineCapacity = 256>
    using TFormatBuffer = Fmt::TInlineFormatBuffer<InlineCapacity>;
}
