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

    // One replacement field's specifier, already parsed. Width and Precision are -1 when absent.
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

    // Growable character sink; the data pointer moves on writes, so never hold one across a write.
    class FFormatBuffer
    {
    public:

        // Required is the length the buffer has to reach, widened so a sum past int32 still reaches the growth check.
        using FGrow = void (*)(FFormatBuffer&, int64);

        FFormatBuffer(char* InData, int32 InCapacity, FGrow InGrow) noexcept
            : Ptr(InData), Cap(InCapacity), GrowFn(InGrow)
        {
        }

        ~FFormatBuffer() = default;
        FFormatBuffer(const FFormatBuffer&) = delete;
        FFormatBuffer& operator=(const FFormatBuffer&) = delete;
        FFormatBuffer(FFormatBuffer&&) = delete;
        FFormatBuffer& operator=(FFormatBuffer&&) = delete;

        template <typename TSelf>
        NODISCARD FORCEINLINE auto GetData(this TSelf&& Self) noexcept -> TLikeSelf<TSelf, char>* { return Self.Ptr; }
        NODISCARD FORCEINLINE int32 Len() const noexcept { return Used; }
        NODISCARD FORCEINLINE int32 GetCapacity() const noexcept { return Cap; }
        NODISCARD FORCEINLINE bool IsEmpty() const noexcept { return Used == 0; }
        NODISCARD FORCEINLINE FStringView View() const noexcept { return FStringView(Ptr, Used); }

        FORCEINLINE void Reset() noexcept { Used = 0; }

        FORCEINLINE void AppendChar(char Character)
        {
            if (Used == Cap) [[unlikely]]
            {
                GrowFn(*this, static_cast<int64>(Used) + 1);
                if (Used == Cap)
                {
                    return;
                }
            }

            Ptr[Used++] = Character;
        }

        FORCEINLINE void Append(const char* Data, int32 Count)
        {
            if (Count <= 0)
            {
                return;
            }

            if (Count > Cap - Used) [[unlikely]]
            {
                AppendGrowing(Data, Count);
                return;
            }

            Memory::Memcpy(Ptr + Used, Data, static_cast<size_t>(Count));
            Used += Count;
        }

        FORCEINLINE void Append(FStringView Text) { Append(Text.GetData(), Text.Len()); }

        void AppendFill(char Character, int32 Count)
        {
            if (Count <= 0)
            {
                return;
            }

            if (Count > Cap - Used)
            {
                GrowFn(*this, static_cast<int64>(Used) + Count);
                Count = Count < Cap - Used ? Count : Cap - Used;
            }

            std::memset(Ptr + Used, Character, static_cast<size_t>(Count));
            Used += Count;
        }

        // Hands back a writable run of Count bytes; follow with Advance once the real length is known.
        FORCEINLINE char* ReserveTail(int32 Count)
        {
            if (Count > Cap - Used)
            {
                GrowFn(*this, static_cast<int64>(Used) + Count);
                LUMINA_CONTAINER_CHECK(Count <= Cap - Used);
            }

            return Ptr + Used;
        }

        FORCEINLINE void Advance(int32 Count) noexcept { Used += Count; }

    protected:

        char*  Ptr;
        int32  Used = 0;
        int32  Cap;
        FGrow  GrowFn;

    private:

        // Growth frees the block Data may point into, so it is re-derived from the new one; a fixed buffer keeps what fits.
        FORCENOINLINE void AppendGrowing(const char* Data, int32 Count)
        {
            const uintptr_t Address = reinterpret_cast<uintptr_t>(Data);
            const uintptr_t Begin = reinterpret_cast<uintptr_t>(Ptr);
            const bool bAliasesSelf = Address >= Begin && Address < Begin + static_cast<uintptr_t>(Used);
            const int32 Offset = bAliasesSelf ? static_cast<int32>(Address - Begin) : 0;

            GrowFn(*this, static_cast<int64>(Used) + Count);

            const char* Source = bAliasesSelf ? Ptr + Offset : Data;
            Count = Count < Cap - Used ? Count : Cap - Used;
            if (Count > 0)
            {
                Memory::Memcpy(Ptr + Used, Source, static_cast<size_t>(Count));
                Used += Count;
            }
        }
    };

    // Formats into a stack buffer and only reaches the allocator when the text outgrows it.
    template <size_t InlineCapacity = 256, ContainerAllocatorType TAllocator = FHeapAllocator>
    class TInlineFormatBuffer final : public FFormatBuffer
    {
    public:

        TInlineFormatBuffer() noexcept : FFormatBuffer(Storage, static_cast<int32>(InlineCapacity), &GrowImpl) {}

        ~TInlineFormatBuffer()
        {
            if (Ptr != Storage)
            {
                TAllocator::Deallocate(Ptr, static_cast<size_t>(Cap), alignof(char));
            }
        }
        TInlineFormatBuffer(const TInlineFormatBuffer&) = delete;
        TInlineFormatBuffer& operator=(const TInlineFormatBuffer&) = delete;
        TInlineFormatBuffer(TInlineFormatBuffer&&) = delete;
        TInlineFormatBuffer& operator=(TInlineFormatBuffer&&) = delete;

    private:

        static void GrowImpl(FFormatBuffer& Base, int64 Required)
        {
            TInlineFormatBuffer& Self = static_cast<TInlineFormatBuffer&>(Base);

            constexpr int64 kMaxCapacity = 0x7FFFFFFF;
            (void)Containers::Private::CheckCapacity(static_cast<uint64>(Required), static_cast<uint64>(kMaxCapacity));
            const int64 Doubled = static_cast<int64>(Self.Cap) * 2;
            const int64 NewCapacity = Doubled < Required ? Required : (Doubled > kMaxCapacity ? kMaxCapacity : Doubled);

            char* Block = static_cast<char*>(TAllocator::Allocate(static_cast<size_t>(NewCapacity), alignof(char)));
            Memory::Memcpy(Block, Self.Ptr, static_cast<size_t>(Self.Used));

            if (Self.Ptr != Self.Storage)
            {
                TAllocator::Deallocate(Self.Ptr, static_cast<size_t>(Self.Cap), alignof(char));
            }

            Self.Ptr = Block;
            Self.Cap = static_cast<int32>(NewCapacity);
        }

        char Storage[InlineCapacity];
    };

    // Writes into a caller-owned array and drops whatever does not fit.
    class FFixedFormatBuffer final : public FFormatBuffer
    {
    public:

        FFixedFormatBuffer(char* InData, int32 InCapacity) noexcept
            : FFormatBuffer(InData, InCapacity, &GrowImpl)
        {
        }

        NODISCARD bool Truncated() const noexcept { return bTruncated; }

    private:

        static void GrowImpl(FFormatBuffer& Base, int64)
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
                int32       Len;
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

    // Anything that is not arithmetic, a pointer or string-like reaches formatting through this.
    template <typename T>
    concept CustomFormattable = requires(FFormatBuffer& Out, const T& Value, const FFormatSpec& Spec)
    {
        FormatArgument(Out, Value, Spec);
    };

    template <typename T>
    concept StringFormattable = std::convertible_to<const T&, FStringView>;

    template <typename T>
    concept StdCharRange = requires(const T& Value)
    {
        { Value.data() } -> std::convertible_to<const char*>;
        { Value.size() } -> std::convertible_to<size_t>;
    };

    template <typename T>
    concept OwnCharRange = requires(const T& Value)
    {
        { Value.GetData() } -> std::convertible_to<const char*>;
        { Value.Len() } -> std::convertible_to<int64>;
    };

    template <typename T>
    concept CharRangeFormattable = !StringFormattable<T> && (StdCharRange<T> || OwnCharRange<T>);

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
            Arg.Int64Value = static_cast<int64>(std::to_underlying(Value));
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
            Arg.StringValue.Data = Text.GetData();
            Arg.StringValue.Len = Text.Len();
            Arg.Type = EFormatArgType::String;
        }
        else if constexpr (CharRangeFormattable<FValue> && !CustomFormattable<FValue>)
        {
            if constexpr (OwnCharRange<FValue>)
            {
                Arg.StringValue.Data = Value.GetData();
                Arg.StringValue.Len = IntCastChecked<int32>(Value.Len());
            }
            else
            {
                Arg.StringValue.Data = Value.data();
                Arg.StringValue.Len = IntCastChecked<int32>(Value.size());
            }
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

        // A literal width or precision of more than six digits is rejected, which keeps the runtime parse far from overflow.
        inline constexpr int32 kMaxFieldDigits = 6;

        NODISCARD constexpr bool SkipFieldDigits(FStringView Fmt, int32& Index) noexcept
        {
            int32 Digits = 0;
            while (Index < Fmt.Len() && IsDigit(Fmt[Index]))
            {
                ++Index;
                ++Digits;
            }
            return Digits <= kMaxFieldDigits;
        }

        // Walks the format string the way the runtime parser does, but only to reject bad input.
        constexpr void ValidateFormatString(FStringView Fmt, const EFormatArgType* Types, size_t TypeCount)
        {
            int32 Index = 0;
            uint32 NextAutomatic = 0;
            bool   bUsedAutomatic = false;
            bool   bUsedManual = false;

            const auto ReadIndex = [&](uint32& OutIndex) constexpr
            {
                if (Index < Fmt.Len() && IsDigit(Fmt[Index]))
                {
                    uint32 Value = 0;
                    while (Index < Fmt.Len() && IsDigit(Fmt[Index]))
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

            while (Index < Fmt.Len())
            {
                const char Character = Fmt[Index];

                if (Character == '}')
                {
                    if (Index + 1 < Fmt.Len() && Fmt[Index + 1] == '}')
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

                if (Index + 1 < Fmt.Len() && Fmt[Index + 1] == '{')
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

                if (Index < Fmt.Len() && Fmt[Index] == ':')
                {
                    ++Index;

                    // Fill only counts as fill when an alignment character follows it.
                    if (Index + 1 < Fmt.Len() &&
                        (Fmt[Index + 1] == '<' || Fmt[Index + 1] == '>' || Fmt[Index + 1] == '^'))
                    {
                        Index += 2;
                    }
                    else if (Index < Fmt.Len() &&
                             (Fmt[Index] == '<' || Fmt[Index] == '>' || Fmt[Index] == '^'))
                    {
                        ++Index;
                    }

                    if (Index < Fmt.Len() && (Fmt[Index] == '+' || Fmt[Index] == '-' || Fmt[Index] == ' '))
                    {
                        ++Index;
                    }

                    if (Index < Fmt.Len() && Fmt[Index] == '#')
                    {
                        ++Index;
                    }

                    if (Index < Fmt.Len() && Fmt[Index] == '0')
                    {
                        ++Index;
                    }

                    if (Index < Fmt.Len() && Fmt[Index] == '{')
                    {
                        ++Index;
                        uint32 WidthIndex = 0;
                        ReadIndex(WidthIndex);
                        HighestIndex = WidthIndex > HighestIndex ? WidthIndex : HighestIndex;
                        if (Index >= Fmt.Len() || Fmt[Index] != '}')
                        {
                            FormatStringError_InvalidSpecifier();
                            return;
                        }
                        ++Index;
                    }
                    else if (!SkipFieldDigits(Fmt, Index))
                    {
                        FormatStringError_InvalidSpecifier();
                        return;
                    }

                    if (Index < Fmt.Len() && Fmt[Index] == '.')
                    {
                        ++Index;
                        if (Index < Fmt.Len() && Fmt[Index] == '{')
                        {
                            ++Index;
                            uint32 PrecisionIndex = 0;
                            ReadIndex(PrecisionIndex);
                            HighestIndex = PrecisionIndex > HighestIndex ? PrecisionIndex : HighestIndex;
                            if (Index >= Fmt.Len() || Fmt[Index] != '}')
                            {
                                FormatStringError_InvalidSpecifier();
                                return;
                            }
                            ++Index;
                        }
                        else if (Index < Fmt.Len() && IsDigit(Fmt[Index]))
                        {
                            if (!SkipFieldDigits(Fmt, Index))
                            {
                                FormatStringError_InvalidSpecifier();
                                return;
                            }
                        }
                        else
                        {
                            FormatStringError_InvalidSpecifier();
                            return;
                        }
                    }

                    if (Index < Fmt.Len() && Fmt[Index] == 'L')
                    {
                        ++Index;
                    }

                    if (Index < Fmt.Len() && Fmt[Index] != '}')
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

                if (Index >= Fmt.Len() || Fmt[Index] != '}')
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

    // A format string checked where it is written, so a bad one is a compile error, not a bad log line.
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

    // Opts a format string out of the compile-time check, for text that only exists at runtime.
    struct FRuntimeFormatString
    {
        FStringView Value;

        explicit constexpr FRuntimeFormatString(FStringView InValue) noexcept : Value(InValue) {}
    };

    NODISCARD FORCEINLINE constexpr FRuntimeFormatString RuntimeFormat(FStringView Text) noexcept
    {
        return FRuntimeFormatString(Text);
    }

    // The parser and every value writer sit behind this one symbol, compiled once for the whole engine.
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
