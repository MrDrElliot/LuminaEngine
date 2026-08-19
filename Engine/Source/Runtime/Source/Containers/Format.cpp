#include "Format.h"

#include <charconv>

namespace Lumina::Fmt
{
    namespace
    {
        constexpr char kPairs[201] =
            "00010203040506070809"
            "10111213141516171819"
            "20212223242526272829"
            "30313233343536373839"
            "40414243444546474849"
            "50515253545556575859"
            "60616263646566676869"
            "70717273747576777879"
            "80818283848586878889"
            "90919293949596979899";

        constexpr char kLowerDigits[17] = "0123456789abcdef";
        constexpr char kUpperDigits[17] = "0123456789ABCDEF";

        constexpr size_t kMaxDigits = 66;

        /** Writes Value backwards ending at End and returns how many characters it took. */
        uint32 WriteDecimalBackwards(char* End, uint64 Value) noexcept
        {
            char* Write = End;

            while (Value >= 100)
            {
                const uint64 Remainder = Value % 100;
                Value /= 100;
                Write -= 2;
                Write[0] = kPairs[Remainder * 2];
                Write[1] = kPairs[Remainder * 2 + 1];
            }

            if (Value >= 10)
            {
                Write -= 2;
                Write[0] = kPairs[Value * 2];
                Write[1] = kPairs[Value * 2 + 1];
            }
            else
            {
                *--Write = static_cast<char>('0' + Value);
            }

            return static_cast<uint32>(End - Write);
        }

        uint32 WritePowerOfTwoBackwards(char* End, uint64 Value, uint32 Shift, const char* Digits) noexcept
        {
            const uint64 Mask = (uint64(1) << Shift) - 1;
            char* Write = End;

            do
            {
                *--Write = Digits[Value & Mask];
                Value >>= Shift;
            }
            while (Value != 0);

            return static_cast<uint32>(End - Write);
        }

        void WritePadded(FFormatBuffer& Out, char SignChar, const char* Prefix, uint32 PrefixLength,
                         const char* Body, uint32 BodyLength, const FFormatSpec& Spec)
        {
            const uint32 SignLength = SignChar != '\0' ? 1u : 0u;
            const uint32 Total = SignLength + PrefixLength + BodyLength;
            const size_t Padding = (Spec.Width > 0 && static_cast<uint32>(Spec.Width) > Total)
                                 ? static_cast<size_t>(Spec.Width) - Total
                                 : 0;

            if (Spec.bZeroPad && Spec.Align == EFormatAlign::Default)
            {
                if (SignChar != '\0')
                {
                    Out.Push(SignChar);
                }

                Out.Append(Prefix, PrefixLength);
                Out.AppendFill('0', Padding);
                Out.Append(Body, BodyLength);
                return;
            }

            size_t Before = 0;
            size_t After = 0;

            switch (Spec.Align)
            {
            case EFormatAlign::Left:   After = Padding; break;
            case EFormatAlign::Center: Before = Padding / 2; After = Padding - Before; break;
            default:                   Before = Padding; break;
            }

            Out.AppendFill(Spec.Fill, Before);

            if (SignChar != '\0')
            {
                Out.Push(SignChar);
            }

            Out.Append(Prefix, PrefixLength);
            Out.Append(Body, BodyLength);
            Out.AppendFill(Spec.Fill, After);
        }

        NODISCARD char SignCharacterFor(bool bNegative, const FFormatSpec& Spec) noexcept
        {
            if (bNegative)
            {
                return '-';
            }

            switch (Spec.Sign)
            {
            case EFormatSign::Plus:  return '+';
            case EFormatSign::Space: return ' ';
            default:                 return '\0';
            }
        }

        void WriteBoolean(FFormatBuffer& Out, bool Value, const FFormatSpec& Spec)
        {
            if (Spec.Type == '\0' || Spec.Type == 's')
            {
                WriteAligned(Out, Value ? FStringView("true", 4) : FStringView("false", 5), Spec);
                return;
            }

            WriteInteger(Out, Value ? 1u : 0u, false, Spec);
        }

        void WriteCharacter(FFormatBuffer& Out, char Value, const FFormatSpec& Spec)
        {
            if (Spec.Type == '\0' || Spec.Type == 'c')
            {
                WriteAligned(Out, FStringView(&Value, 1), Spec);
                return;
            }

            WriteInteger(Out, static_cast<uint64>(static_cast<unsigned char>(Value)), false, Spec);
        }

        void WritePointer(FFormatBuffer& Out, const void* Value, const FFormatSpec& Spec)
        {
            char Digits[kMaxDigits];
            char* const End = Digits + kMaxDigits;

            const bool bUpper = Spec.Type == 'P';
            const uint64 Address = static_cast<uint64>(reinterpret_cast<uintptr_t>(Value));
            const uint32 Length = WritePowerOfTwoBackwards(End, Address, 4, bUpper ? kUpperDigits : kLowerDigits);

            WritePadded(Out, '\0', bUpper ? "0X" : "0x", 2, End - Length, Length, Spec);
        }

        NODISCARD size_t StringLength(const char* Text) noexcept
        {
            if (Text == nullptr)
            {
                return 0;
            }

            const char* Scan = Text;
            while (*Scan != '\0')
            {
                ++Scan;
            }

            return static_cast<size_t>(Scan - Text);
        }

        NODISCARD bool IsDigitChar(char Character) noexcept
        {
            return Character >= '0' && Character <= '9';
        }

        void WriteArgument(FFormatBuffer& Out, const FFormatArg& Arg, const FFormatSpec& Spec)
        {
            switch (Arg.Type)
            {
            case EFormatArgType::Bool:
                WriteBoolean(Out, Arg.BoolValue, Spec);
                break;
            case EFormatArgType::Char:
                WriteCharacter(Out, Arg.CharValue, Spec);
                break;
            case EFormatArgType::Int64:
            {
                const bool bNegative = Arg.Int64Value < 0;
                const uint64 Magnitude = bNegative
                                       ? (~static_cast<uint64>(Arg.Int64Value) + 1)
                                       : static_cast<uint64>(Arg.Int64Value);
                if (Spec.Type == 'c')
                {
                    WriteCharacter(Out, static_cast<char>(Arg.Int64Value), FFormatSpec{ Spec.Width, -1, Spec.Fill,
                                   'c', Spec.Align, Spec.Sign, false, false });
                }
                else
                {
                    WriteInteger(Out, Magnitude, bNegative, Spec);
                }
                break;
            }
            case EFormatArgType::UInt64:
                if (Spec.Type == 'c')
                {
                    WriteCharacter(Out, static_cast<char>(Arg.UInt64Value), FFormatSpec{ Spec.Width, -1, Spec.Fill,
                                   'c', Spec.Align, Spec.Sign, false, false });
                }
                else
                {
                    WriteInteger(Out, Arg.UInt64Value, false, Spec);
                }
                break;
            case EFormatArgType::Double:
                WriteFloat(Out, Arg.DoubleValue, Spec);
                break;
            case EFormatArgType::CString:
                WriteAligned(Out, FStringView(Arg.CStringValue, StringLength(Arg.CStringValue)), Spec);
                break;
            case EFormatArgType::String:
                WriteAligned(Out, FStringView(Arg.StringValue.Data, Arg.StringValue.Size), Spec);
                break;
            case EFormatArgType::Pointer:
                WritePointer(Out, Arg.PointerValue, Spec);
                break;
            case EFormatArgType::Custom:
                Arg.CustomValue.Thunk(Out, Arg.CustomValue.Value, Spec);
                break;
            default:
                Out.Append("<none>", 6);
                break;
            }
        }

        NODISCARD int32 ArgAsWidth(const FFormatArg& Arg) noexcept
        {
            switch (Arg.Type)
            {
            case EFormatArgType::Int64:  return static_cast<int32>(Arg.Int64Value);
            case EFormatArgType::UInt64: return static_cast<int32>(Arg.UInt64Value);
            default:                     return -1;
            }
        }
    }

    void WriteAligned(FFormatBuffer& Out, FStringView Text, const FFormatSpec& Spec)
    {
        size_t Length = Text.size();
        if (Spec.Precision >= 0 && static_cast<size_t>(Spec.Precision) < Length)
        {
            Length = static_cast<size_t>(Spec.Precision);
        }

        const size_t Padding = (Spec.Width > 0 && static_cast<size_t>(Spec.Width) > Length)
                             ? static_cast<size_t>(Spec.Width) - Length
                             : 0;

        size_t Before = 0;
        size_t After = 0;

        switch (Spec.Align)
        {
        case EFormatAlign::Right:  Before = Padding; break;
        case EFormatAlign::Center: Before = Padding / 2; After = Padding - Before; break;
        default:                   After = Padding; break;
        }

        Out.AppendFill(Spec.Fill, Before);
        Out.Append(Text.data(), Length);
        Out.AppendFill(Spec.Fill, After);
    }

    void WriteInteger(FFormatBuffer& Out, uint64 Magnitude, bool bNegative, const FFormatSpec& Spec)
    {
        char Digits[kMaxDigits];
        char* const End = Digits + kMaxDigits;

        const char* Prefix = "";
        uint32 PrefixLength = 0;
        uint32 Length = 0;

        switch (Spec.Type)
        {
        case 'b':
            Length = WritePowerOfTwoBackwards(End, Magnitude, 1, kLowerDigits);
            Prefix = "0b";
            PrefixLength = Spec.bAlternate ? 2u : 0u;
            break;
        case 'B':
            Length = WritePowerOfTwoBackwards(End, Magnitude, 1, kUpperDigits);
            Prefix = "0B";
            PrefixLength = Spec.bAlternate ? 2u : 0u;
            break;
        case 'o':
            Length = WritePowerOfTwoBackwards(End, Magnitude, 3, kLowerDigits);
            Prefix = "0";
            PrefixLength = (Spec.bAlternate && Magnitude != 0) ? 1u : 0u;
            break;
        case 'x':
            Length = WritePowerOfTwoBackwards(End, Magnitude, 4, kLowerDigits);
            Prefix = "0x";
            PrefixLength = Spec.bAlternate ? 2u : 0u;
            break;
        case 'X':
            Length = WritePowerOfTwoBackwards(End, Magnitude, 4, kUpperDigits);
            Prefix = "0X";
            PrefixLength = Spec.bAlternate ? 2u : 0u;
            break;
        default:
            Length = WriteDecimalBackwards(End, Magnitude);
            break;
        }

        WritePadded(Out, SignCharacterFor(bNegative, Spec), Prefix, PrefixLength, End - Length, Length, Spec);
    }

    void WriteFloat(FFormatBuffer& Out, double Value, const FFormatSpec& Spec)
    {
        const bool bNegative = Value < 0.0 || (Value == 0.0 && std::signbit(Value));
        const double Magnitude = bNegative ? -Value : Value;
        const char SignChar = SignCharacterFor(bNegative, Spec);

        if (Magnitude != Magnitude)
        {
            const bool bUpper = Spec.Type == 'E' || Spec.Type == 'F' || Spec.Type == 'G' || Spec.Type == 'A';
            WritePadded(Out, SignChar, "", 0, bUpper ? "NAN" : "nan", 3, Spec);
            return;
        }

        if (Magnitude > 1.7976931348623157e308)
        {
            const bool bUpper = Spec.Type == 'E' || Spec.Type == 'F' || Spec.Type == 'G' || Spec.Type == 'A';
            WritePadded(Out, SignChar, "", 0, bUpper ? "INF" : "inf", 3, Spec);
            return;
        }

        std::chars_format Format = std::chars_format::general;
        bool bHasFormat = true;
        bool bUpper = false;

        switch (Spec.Type)
        {
        case 'a': Format = std::chars_format::hex; break;
        case 'A': Format = std::chars_format::hex; bUpper = true; break;
        case 'e': Format = std::chars_format::scientific; break;
        case 'E': Format = std::chars_format::scientific; bUpper = true; break;
        case 'f': Format = std::chars_format::fixed; break;
        case 'F': Format = std::chars_format::fixed; bUpper = true; break;
        case 'g': Format = std::chars_format::general; break;
        case 'G': Format = std::chars_format::general; bUpper = true; break;
        default:  bHasFormat = false; break;
        }

        const int32 Precision = Spec.Precision >= 0 ? Spec.Precision : 6;

        char Stack[512];
        char* Scratch = Stack;
        size_t ScratchSize = sizeof(Stack);
        char* Heap = nullptr;

        std::to_chars_result Result{};
        while (true)
        {
            if (!bHasFormat && Spec.Precision < 0)
            {
                Result = std::to_chars(Scratch, Scratch + ScratchSize, Magnitude);
            }
            else if (!bHasFormat)
            {
                Result = std::to_chars(Scratch, Scratch + ScratchSize, Magnitude,
                                       std::chars_format::general, Precision);
            }
            else
            {
                Result = std::to_chars(Scratch, Scratch + ScratchSize, Magnitude, Format, Precision);
            }

            if (Result.ec == std::errc{})
            {
                break;
            }

            ScratchSize *= 4;
            if (Heap != nullptr)
            {
                FHeapAllocator::Deallocate(Heap, ScratchSize / 4, alignof(char));
            }

            Heap = static_cast<char*>(FHeapAllocator::Allocate(ScratchSize, alignof(char)));
            Scratch = Heap;
        }

        uint32 Length = static_cast<uint32>(Result.ptr - Scratch);

        if (bUpper)
        {
            for (uint32 Index = 0; Index < Length; ++Index)
            {
                char& Character = Scratch[Index];
                if (Character >= 'a' && Character <= 'z')
                {
                    Character = static_cast<char>(Character - 'a' + 'A');
                }
            }
        }

        WritePadded(Out, SignChar, "", 0, Scratch, Length, Spec);

        if (Heap != nullptr)
        {
            FHeapAllocator::Deallocate(Heap, ScratchSize, alignof(char));
        }
    }

    void VFormatTo(FFormatBuffer& Out, FStringView Fmt, FFormatArgs Args)
    {
        const char* const Begin = Fmt.data();
        const size_t Count = Fmt.size();

        size_t Index = 0;
        size_t Literal = 0;
        uint32 NextAutomatic = 0;

        const auto ReadArgIndex = [&]() -> uint32
        {
            if (Index < Count && IsDigitChar(Begin[Index]))
            {
                uint32 Value = 0;
                while (Index < Count && IsDigitChar(Begin[Index]))
                {
                    Value = Value * 10 + static_cast<uint32>(Begin[Index] - '0');
                    ++Index;
                }

                return Value;
            }

            return NextAutomatic++;
        };

        while (Index < Count)
        {
            const char Character = Begin[Index];

            if (Character != '{' && Character != '}')
            {
                ++Index;
                continue;
            }

            Out.Append(Begin + Literal, Index - Literal);

            if (Index + 1 < Count && Begin[Index + 1] == Character)
            {
                Out.Push(Character);
                Index += 2;
                Literal = Index;
                continue;
            }

            if (Character == '}')
            {
                Out.Append("<stray }>", 9);
                return;
            }

            ++Index;

            const uint32 ArgIndex = ReadArgIndex();
            FFormatSpec Spec;

            if (Index < Count && Begin[Index] == ':')
            {
                ++Index;

                if (Index + 1 < Count &&
                    (Begin[Index + 1] == '<' || Begin[Index + 1] == '>' || Begin[Index + 1] == '^'))
                {
                    Spec.Fill = Begin[Index];
                    Spec.Align = Begin[Index + 1] == '<' ? EFormatAlign::Left
                               : Begin[Index + 1] == '>' ? EFormatAlign::Right
                                                         : EFormatAlign::Center;
                    Index += 2;
                }
                else if (Index < Count &&
                         (Begin[Index] == '<' || Begin[Index] == '>' || Begin[Index] == '^'))
                {
                    Spec.Align = Begin[Index] == '<' ? EFormatAlign::Left
                               : Begin[Index] == '>' ? EFormatAlign::Right
                                                     : EFormatAlign::Center;
                    ++Index;
                }

                if (Index < Count)
                {
                    if (Begin[Index] == '+')      { Spec.Sign = EFormatSign::Plus;  ++Index; }
                    else if (Begin[Index] == '-') { Spec.Sign = EFormatSign::Minus; ++Index; }
                    else if (Begin[Index] == ' ') { Spec.Sign = EFormatSign::Space; ++Index; }
                }

                if (Index < Count && Begin[Index] == '#')
                {
                    Spec.bAlternate = true;
                    ++Index;
                }

                if (Index < Count && Begin[Index] == '0')
                {
                    Spec.bZeroPad = true;
                    Spec.Fill = '0';
                    ++Index;
                }

                if (Index < Count && Begin[Index] == '{')
                {
                    ++Index;
                    const uint32 WidthIndex = ReadArgIndex();
                    if (WidthIndex < Args.Count)
                    {
                        Spec.Width = ArgAsWidth(Args.Values[WidthIndex]);
                    }

                    if (Index < Count && Begin[Index] == '}')
                    {
                        ++Index;
                    }
                }
                else
                {
                    int32 Width = 0;
                    bool bHasWidth = false;
                    while (Index < Count && IsDigitChar(Begin[Index]))
                    {
                        Width = Width * 10 + (Begin[Index] - '0');
                        bHasWidth = true;
                        ++Index;
                    }

                    if (bHasWidth)
                    {
                        Spec.Width = Width;
                    }
                }

                if (Index < Count && Begin[Index] == '.')
                {
                    ++Index;
                    if (Index < Count && Begin[Index] == '{')
                    {
                        ++Index;
                        const uint32 PrecisionIndex = ReadArgIndex();
                        if (PrecisionIndex < Args.Count)
                        {
                            Spec.Precision = ArgAsWidth(Args.Values[PrecisionIndex]);
                        }

                        if (Index < Count && Begin[Index] == '}')
                        {
                            ++Index;
                        }
                    }
                    else
                    {
                        int32 Precision = 0;
                        while (Index < Count && IsDigitChar(Begin[Index]))
                        {
                            Precision = Precision * 10 + (Begin[Index] - '0');
                            ++Index;
                        }

                        Spec.Precision = Precision;
                    }
                }

                if (Index < Count && Begin[Index] == 'L')
                {
                    ++Index;
                }

                if (Index < Count && Begin[Index] != '}')
                {
                    Spec.Type = Begin[Index];
                    ++Index;
                }
            }

            if (Index >= Count || Begin[Index] != '}')
            {
                Out.Append("<unclosed {>", 12);
                return;
            }

            ++Index;
            Literal = Index;

            if (ArgIndex < Args.Count)
            {
                WriteArgument(Out, Args.Values[ArgIndex], Spec);
            }
            else
            {
                Out.Append("<missing arg>", 13);
            }
        }

        Out.Append(Begin + Literal, Count - Literal);
    }
}
