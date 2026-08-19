#pragma once

#include <type_traits>
#include <utility>

#include "Format.h"
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
        TStringBuilder& AppendFormat(Fmt::TFormatString<std::decay_t<TArgs>...> Fmt, TArgs&&... Args)
        {
            const Fmt::TFormatArgStore<std::decay_t<TArgs>...> Store(Args...);
            Fmt::VFormatTo(Buffer, Fmt.Get(), Store.View());
            return *this;
        }

        TStringBuilder& AppendFormat(Fmt::FRuntimeFormatString Fmt)
        {
            Fmt::VFormatTo(Buffer, Fmt.Value, Fmt::FFormatArgs{});
            return *this;
        }

        TStringBuilder& Append(FStringView Text)
        {
            Buffer.Append(Text.data(), Text.size());
            return *this;
        }

        TStringBuilder& Append(char Character)
        {
            Buffer.Push(Character);
            return *this;
        }

        TStringBuilder& AppendLine(FStringView Text = FStringView())
        {
            Append(Text);
            Buffer.Push('\n');
            return *this;
        }

        // The terminator lives one past the size, inside reserved capacity, so appending can resume after this.
        NODISCARD const char* c_str()
        {
            Buffer.ReserveTail(1)[0] = '\0';
            return Buffer.Data();
        }

        NODISCARD FStringView View() const { return FStringView(Buffer.Data(), Buffer.Size()); }
        NODISCARD FString ToString() const { return FString(Buffer.Data(), Buffer.Size()); }

        NODISCARD const char* data() const { return Buffer.Data(); }
        NODISCARD size_t size() const { return Buffer.Size(); }
        NODISCARD bool empty() const { return Buffer.Empty(); }

        void Reset() { Buffer.Clear(); }

    private:

        Fmt::TInlineFormatBuffer<InlineCapacity> Buffer;
    };

    using FStringBuilder = TStringBuilder<256>;

    /** Compile-time checked formatting, replacing the printf-style FString::sprintf. */
    template <typename... TArgs>
    NODISCARD FString Format(Fmt::TFormatString<std::decay_t<TArgs>...> Fmt, TArgs&&... Args)
    {
        Fmt::TInlineFormatBuffer<256> Buffer;
        const Fmt::TFormatArgStore<std::decay_t<TArgs>...> Store(Args...);
        Fmt::VFormatTo(Buffer, Fmt.Get(), Store.View());
        return FString(Buffer.Data(), Buffer.Size());
    }

    /** Formats a string that only exists at runtime, so nothing is checked at the call site. */
    template <typename... TArgs>
    NODISCARD FString Format(Fmt::FRuntimeFormatString Fmt, TArgs&&... Args)
    {
        Fmt::TInlineFormatBuffer<256> Buffer;
        const Fmt::TFormatArgStore<std::decay_t<TArgs>...> Store(Args...);
        Fmt::VFormatTo(Buffer, Fmt.Value, Store.View());
        return FString(Buffer.Data(), Buffer.Size());
    }

    /** Formats into a chosen string type, so a fixed-capacity target keeps the result off the heap. */
    template <typename TOut, typename... TArgs>
    NODISCARD TOut FormatAs(Fmt::TFormatString<std::decay_t<TArgs>...> Fmt, TArgs&&... Args)
    {
        Fmt::TInlineFormatBuffer<256> Buffer;
        const Fmt::TFormatArgStore<std::decay_t<TArgs>...> Store(Args...);
        Fmt::VFormatTo(Buffer, Fmt.Get(), Store.View());
        return TOut(Buffer.Data(), Buffer.Size());
    }

    /** Appends straight into a format buffer, with no staging copy in between. */
    template <typename... TArgs>
    void AppendFormat(Fmt::FFormatBuffer& Out, Fmt::TFormatString<std::decay_t<TArgs>...> Fmt, TArgs&&... Args)
    {
        const Fmt::TFormatArgStore<std::decay_t<TArgs>...> Store(Args...);
        Fmt::VFormatTo(Out, Fmt.Get(), Store.View());
    }

    /** Appends to any string that can append, replacing FString::append_sprintf. */
    template <typename TOut, typename... TArgs>
    requires (!std::is_base_of_v<Fmt::FFormatBuffer, TOut>)
    void AppendFormat(TOut& Out, Fmt::TFormatString<std::decay_t<TArgs>...> Fmt, TArgs&&... Args)
    {
        Fmt::TInlineFormatBuffer<256> Buffer;
        const Fmt::TFormatArgStore<std::decay_t<TArgs>...> Store(Args...);
        Fmt::VFormatTo(Buffer, Fmt.Get(), Store.View());
        Out.append(Buffer.Data(), Buffer.Size());
    }

    /** Replaces the contents of any string that can append, replacing FString::sprintf. */
    template <typename TOut, typename... TArgs>
    void FormatTo(TOut& Out, Fmt::TFormatString<std::decay_t<TArgs>...> Fmt, TArgs&&... Args)
    {
        Out.clear();
        AppendFormat(Out, Fmt, std::forward<TArgs>(Args)...);
    }

    /** Formats into a caller-owned array and reports how much was written, never allocating. */
    template <typename... TArgs>
    size_t FormatToBuffer(char* Out, size_t Capacity, Fmt::TFormatString<std::decay_t<TArgs>...> Fmt, TArgs&&... Args)
    {
        Fmt::FFixedFormatBuffer Buffer(Out, Capacity);
        const Fmt::TFormatArgStore<std::decay_t<TArgs>...> Store(Args...);
        Fmt::VFormatTo(Buffer, Fmt.Get(), Store.View());
        return Buffer.Size();
    }
}
