#pragma once

#include <cstring>

#include "LogLevel.h"
#include "LogSink.h"
#include "Memory/Memory.h"


namespace Lumina::Logging
{
    // Timestamps are almost all two-digit fields; a lookup beats a divide per digit.
    struct FTwoDigitTable
    {
        char Data[200] = {};

        constexpr FTwoDigitTable()
        {
            for (uint32 Value = 0; Value < 100; ++Value)
            {
                Data[Value * 2 + 0] = static_cast<char>('0' + Value / 10);
                Data[Value * 2 + 1] = static_cast<char>('0' + Value % 10);
            }
        }
    };

    inline constexpr FTwoDigitTable GTwoDigits{};


    // Append-only. A whole batch is assembled here and handed to the OS in one write.
    class FLogBuffer
    {
    public:

        explicit FLogBuffer(uint32 InitialCapacity = 64 * 1024)
        {
            Storage  = static_cast<char*>(Memory::Malloc(InitialCapacity));
            Capacity = InitialCapacity;
        }

        ~FLogBuffer()
        {
            void* Ptr = Storage;
            Memory::Free(Ptr);
            Storage = nullptr;
        }

        FLogBuffer(const FLogBuffer&) = delete;
        FLogBuffer& operator=(const FLogBuffer&) = delete;

        NODISCARD const char* Data() const noexcept { return Storage; }
        NODISCARD uint32 Size() const noexcept { return Length; }
        NODISCARD bool IsEmpty() const noexcept { return Length == 0; }

        void Clear() noexcept { Length = 0; }

        void Append(const char* Text, uint32 Count)
        {
            if (Count == 0)
            {
                return;
            }

            Reserve(Count);
            std::memcpy(Storage + Length, Text, Count);
            Length += Count;
        }

        void Append(FStringView Text)
        {
            Append(Text.data(), static_cast<uint32>(Text.size()));
        }

        template<uint32 N>
        void AppendLiteral(const char (&Literal)[N])
        {
            Append(Literal, N - 1);
        }

        void AppendChar(char Value)
        {
            Reserve(1);
            Storage[Length++] = Value;
        }

        void AppendTwoDigits(uint32 Value)
        {
            Reserve(2);
            std::memcpy(Storage + Length, GTwoDigits.Data + (Value % 100) * 2, 2);
            Length += 2;
        }

        void AppendThreeDigits(uint32 Value)
        {
            Reserve(3);
            Storage[Length++] = static_cast<char>('0' + (Value / 100) % 10);
            std::memcpy(Storage + Length, GTwoDigits.Data + (Value % 100) * 2, 2);
            Length += 2;
        }

        void AppendUInt(uint64 Value)
        {
            char Digits[20];
            uint32 Written = 0;
            do
            {
                Digits[Written++] = static_cast<char>('0' + Value % 10);
                Value /= 10;
            }
            while (Value != 0);

            Reserve(Written);
            while (Written > 0)
            {
                Storage[Length++] = Digits[--Written];
            }
        }

    private:

        void Reserve(uint32 Extra)
        {
            if (Length + Extra <= Capacity) [[likely]]
            {
                return;
            }

            uint32 NewCapacity = Capacity * 2;
            while (NewCapacity < Length + Extra)
            {
                NewCapacity *= 2;
            }

            char* NewStorage = static_cast<char*>(Memory::Malloc(NewCapacity));
            std::memcpy(NewStorage, Storage, Length);

            void* Old = Storage;
            Memory::Free(Old);

            Storage  = NewStorage;
            Capacity = NewCapacity;
        }

        char*  Storage  = nullptr;
        uint32 Capacity = 0;
        uint32 Length   = 0;
    };


    inline void AppendClock(FLogBuffer& Out, const FLogTimestamp& Stamp)
    {
        Out.Append(Stamp.Clock, 8);
    }

    inline void AppendDateTime(FLogBuffer& Out, const FLogTimestamp& Stamp)
    {
        Out.Append(Stamp.Date, 10);
        Out.AppendChar(' ');
        Out.Append(Stamp.Clock, 8);
        Out.AppendChar('.');
        Out.AppendThreeDigits(Stamp.Millis);
    }
}
