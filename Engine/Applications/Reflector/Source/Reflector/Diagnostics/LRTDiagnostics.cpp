#include "LRTDiagnostics.h"

#include <clang-c/CXSourceLocation.h>
#include <clang-c/CXString.h>
#include <cstdio>
#include <algorithm>

namespace Lumina::Reflection
{
    FDiagLocation MakeLocationFromCursor(const CXCursor& Cursor)
    {
        FDiagLocation Result;

        const CXSourceLocation Loc = clang_getCursorLocation(Cursor);
        CXFile   File   = nullptr;
        uint32_t Line   = 0;
        uint32_t Column = 0;
        clang_getExpansionLocation(Loc, &File, &Line, &Column, nullptr);

        if (File != nullptr)
        {
            CXString Name = clang_getFileName(File);
            const char* Raw = clang_getCString(Name);
            if (Raw != nullptr)
            {
                Result.File = Raw;
                std::replace(Result.File.begin(), Result.File.end(), '\\', '/');
            }
            clang_disposeString(Name);
        }

        Result.Line   = Line;
        Result.Column = Column;
        return Result;
    }

    FDiagnostics& FDiagnostics::Get()
    {
        static FDiagnostics Instance;
        return Instance;
    }

    void FDiagnostics::Emit(const char* Severity, const FDiagLocation& Loc, EDiagId Id, const char* Message)
    {
        // Without a usable location, fall back to a bare prefix so the line still parses for MSBuild.
        if (!Loc.File.empty() && Loc.Line != 0)
        {
            std::fprintf(stderr,
                "%s(%u,%u): %s LRT%04u: %s\n",
                Loc.File.c_str(), Loc.Line, Loc.Column,
                Severity, static_cast<uint32_t>(Id), Message);
        }
        else
        {
            std::fprintf(stderr, "LRT: %s LRT%04u: %s\n", Severity, static_cast<uint32_t>(Id), Message);
        }
        std::fflush(stderr);
    }

    namespace
    {
        std::string FormatV(const char* Fmt, va_list Args)
        {
            va_list Copy;
            va_copy(Copy, Args);
            const int Needed = std::vsnprintf(nullptr, 0, Fmt, Copy);
            va_end(Copy);

            if (Needed <= 0)
            {
                return {};
            }

            std::string Result;
            Result.resize(static_cast<size_t>(Needed));
            std::vsnprintf(Result.data(), static_cast<size_t>(Needed) + 1, Fmt, Args);
            return Result;
        }
    }

    void FDiagnostics::Errorf(const FDiagLocation& Loc, EDiagId Id, const char* Fmt, ...)
    {
        va_list Args;
        va_start(Args, Fmt);
        const std::string Msg = FormatV(Fmt, Args);
        va_end(Args);

        ++ErrorCount;
        Emit("error", Loc, Id, Msg.c_str());
    }

    void FDiagnostics::Warningf(const FDiagLocation& Loc, EDiagId Id, const char* Fmt, ...)
    {
        va_list Args;
        va_start(Args, Fmt);
        const std::string Msg = FormatV(Fmt, Args);
        va_end(Args);

        ++WarningCount;
        Emit("warning", Loc, Id, Msg.c_str());
    }

    void FDiagnostics::PrintSummary() const
    {
        if (ErrorCount == 0 && WarningCount == 0)
        {
            return;
        }
        std::fprintf(stderr, "[LRT] %u error(s), %u warning(s)\n", ErrorCount, WarningCount);
        std::fflush(stderr);
    }
}
