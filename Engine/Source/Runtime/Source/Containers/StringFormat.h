#pragma once

#include <format>
#include <iterator>
#include <utility>

#include "String.h"
#include "Vector.h"

namespace Lumina
{
    /** Builds text into an inline buffer, so the usual case formats without reaching the allocator. */
    template <size_t InlineCapacity = 256>
    class TStringBuilder
    {
    public:

        TStringBuilder() = default;
        explicit TStringBuilder(FStringView Initial) { Append(Initial); }

        template <typename... TArgs>
        TStringBuilder& AppendFormat(std::format_string<TArgs...> Fmt, TArgs&&... Args)
        {
            std::format_to(std::back_inserter(Buffer), Fmt, std::forward<TArgs>(Args)...);
            return *this;
        }

        TStringBuilder& Append(FStringView Text)
        {
            Buffer.Append(Text.data(), Text.data() + Text.size());
            return *this;
        }

        TStringBuilder& Append(char Character)
        {
            Buffer.push_back(Character);
            return *this;
        }

        TStringBuilder& AppendLine(FStringView Text = FStringView())
        {
            Append(Text);
            Buffer.push_back('\n');
            return *this;
        }

        // The terminator lives one past the size, inside reserved capacity, so appending can resume after this.
        NODISCARD const char* c_str()
        {
            Buffer.Reserve(Buffer.size() + 1);
            Buffer.data()[Buffer.size()] = '\0';
            return Buffer.data();
        }

        NODISCARD FStringView View() const { return FStringView(Buffer.data(), Buffer.size()); }
        NODISCARD FString ToString() const { return FString(Buffer.data(), Buffer.size()); }

        NODISCARD const char* data() const { return Buffer.data(); }
        NODISCARD size_t size() const { return Buffer.size(); }
        NODISCARD bool empty() const { return Buffer.empty(); }

        void Reset() { Buffer.clear(); }
        void Reserve(size_t Characters) { Buffer.Reserve(Characters); }

    private:

        Containers::TInlineVector<char, InlineCapacity> Buffer;
    };

    using FStringBuilder = TStringBuilder<256>;

    /** Compile-time checked formatting, replacing the printf-style FString::sprintf. */
    template <typename... TArgs>
    NODISCARD FString Format(std::format_string<TArgs...> Fmt, TArgs&&... Args)
    {
        FString Result;
        std::format_to(std::back_inserter(Result), Fmt, std::forward<TArgs>(Args)...);
        return Result;
    }

    /** Formats into a chosen string type, so a fixed-capacity target keeps the result off the heap. */
    template <typename TOut, typename... TArgs>
    NODISCARD TOut FormatAs(std::format_string<TArgs...> Fmt, TArgs&&... Args)
    {
        TOut Result;
        std::format_to(std::back_inserter(Result), Fmt, std::forward<TArgs>(Args)...);
        return Result;
    }

    /** Appends to any string that can push_back, replacing FString::append_sprintf. */
    template <typename TOut, typename... TArgs>
    void AppendFormat(TOut& Out, std::format_string<TArgs...> Fmt, TArgs&&... Args)
    {
        std::format_to(std::back_inserter(Out), Fmt, std::forward<TArgs>(Args)...);
    }

    /** Replaces the contents of any string that can push_back, replacing FString::sprintf. */
    template <typename TOut, typename... TArgs>
    void FormatTo(TOut& Out, std::format_string<TArgs...> Fmt, TArgs&&... Args)
    {
        Out.clear();
        std::format_to(std::back_inserter(Out), Fmt, std::forward<TArgs>(Args)...);
    }
}
