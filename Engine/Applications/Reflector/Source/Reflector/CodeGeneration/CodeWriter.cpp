#include "CodeWriter.h"

#include <cstdio>

namespace Lumina::Reflection
{
    namespace
    {
        constexpr size_t kDefaultCapacity = 16llu * 1024;
        constexpr size_t kSmallBufferSize = 512llu;
    }

    FCodeWriter::FCodeWriter()
    {
        Buffer.reserve(kDefaultCapacity);
    }

    FCodeWriter::FCodeWriter(size_t InitialCapacity)
    {
        Buffer.reserve(InitialCapacity);
    }

    void FCodeWriter::Clear()
    {
        Buffer.clear();
        IndentLevel = 0;
    }

    void FCodeWriter::WriteIndent()
    {
        for (int i = 0; i < IndentLevel; ++i)
        {
            Buffer.push_back('\t');
        }
    }

    void FCodeWriter::AppendVaList(const char* Fmt, va_list Args)
    {
        va_list Copy;
        va_copy(Copy, Args);

        char Small[kSmallBufferSize];
        const int Needed = std::vsnprintf(Small, sizeof(Small), Fmt, Copy);
        va_end(Copy);

        if (Needed < 0)
        {
            return;
        }

        if (static_cast<size_t>(Needed) < sizeof(Small))
        {
            Buffer.append(Small, Small + Needed);
            return;
        }

        const size_t Size = static_cast<size_t>(Needed) + 1;
        std::string Scratch;
        Scratch.resize(Size);
        std::vsnprintf(Scratch.data(), Size, Fmt, Args);
        Buffer.append(Scratch.data(), Scratch.data() + Needed);
    }

    FCodeWriter& FCodeWriter::Append(std::string_view Text)
    {
        Buffer.append(Text.data(), Text.data() + Text.size());
        return *this;
    }

    FCodeWriter& FCodeWriter::Append(const char* Text)
    {
        if (Text != nullptr)
        {
            Buffer.append(Text);
        }
        return *this;
    }

    FCodeWriter& FCodeWriter::Append(const std::string& Text)
    {
        Buffer.append(Text);
        return *this;
    }

    FCodeWriter& FCodeWriter::Appendf(const char* Fmt, ...)
    {
        va_list Args;
        va_start(Args, Fmt);
        AppendVaList(Fmt, Args);
        va_end(Args);
        return *this;
    }

    FCodeWriter& FCodeWriter::Line()
    {
        Buffer.push_back('\n');
        return *this;
    }

    FCodeWriter& FCodeWriter::Line(std::string_view Text)
    {
        WriteIndent();
        Buffer.append(Text.data(), Text.data() + Text.size());
        Buffer.push_back('\n');
        return *this;
    }

    FCodeWriter& FCodeWriter::Line(const char* Text)
    {
        return Line(std::string_view(Text != nullptr ? Text : ""));
    }

    FCodeWriter& FCodeWriter::Line(const std::string& Text)
    {
        return Line(std::string_view(Text.data(), Text.size()));
    }

    FCodeWriter& FCodeWriter::Linef(const char* Fmt, ...)
    {
        WriteIndent();
        va_list Args;
        va_start(Args, Fmt);
        AppendVaList(Fmt, Args);
        va_end(Args);
        Buffer.push_back('\n');
        return *this;
    }

    FCodeWriter& FCodeWriter::BlankLines(int Count)
    {
        for (int i = 0; i < Count; ++i)
        {
            Buffer.push_back('\n');
        }
        return *this;
    }

    FCodeWriter& FCodeWriter::Macro(std::string_view Text)
    {
        WriteIndent();
        Buffer.append(Text.data(), Text.data() + Text.size());
        Buffer.append(" \\\n");
        return *this;
    }

    FCodeWriter& FCodeWriter::Macro(const char* Text)
    {
        return Macro(std::string_view(Text != nullptr ? Text : ""));
    }

    FCodeWriter& FCodeWriter::Macrof(const char* Fmt, ...)
    {
        WriteIndent();
        va_list Args;
        va_start(Args, Fmt);
        AppendVaList(Fmt, Args);
        va_end(Args);
        Buffer.append(" \\\n");
        return *this;
    }

    void FCodeWriter::FinalizeMacro()
    {
        // Strip a trailing " \\\n" if present so the last macro line has no continuation.
        if (Buffer.size() >= 3 &&
            Buffer[Buffer.size() - 3] == ' ' &&
            Buffer[Buffer.size() - 2] == '\\' &&
            Buffer[Buffer.size() - 1] == '\n')
        {
            Buffer.resize(Buffer.size() - 3);
            Buffer.push_back('\n');
        }
    }

    FCodeWriter& FCodeWriter::BeginBlock()
    {
        Line("{");
        PushIndent();
        return *this;
    }

    FCodeWriter& FCodeWriter::EndBlock()
    {
        PopIndent();
        Line("}");
        return *this;
    }

    FCodeWriter& FCodeWriter::EndBlockSemi()
    {
        PopIndent();
        Line("};");
        return *this;
    }
}
