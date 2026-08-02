#include "RuntimePCH.h"
#include "StdoutSink.h"

#include <cstdio>

#if defined(_WIN32)
    #include <io.h>
    #include <windows.h>
#endif

namespace Lumina
{
    namespace
    {
        // Redirected output gets clean text instead of escape soup.
        bool EnableVirtualTerminal()
        {
        #if defined(_WIN32)
            if (_isatty(_fileno(stdout)) == 0)
            {
                return false;
            }

            const HANDLE Out = GetStdHandle(STD_OUTPUT_HANDLE);
            if (Out == nullptr || Out == INVALID_HANDLE_VALUE)
            {
                return false;
            }

            DWORD Mode = 0;
            if (!GetConsoleMode(Out, &Mode))
            {
                return false;
            }

            return SetConsoleMode(Out, Mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
        #else
            return isatty(fileno(stdout)) != 0;
        #endif
        }
    }

    FStdoutSink::FStdoutSink()
        : Batch(64 * 1024)
        , bUseColor(EnableVirtualTerminal())
    {
        std::setvbuf(stdout, nullptr, _IOFBF, 64 * 1024);
    }

    void FStdoutSink::Write(const Logging::FLogRecord& Record)
    {
        const Logging::FLevelDescriptor& Descriptor = Logging::GetLevelDescriptor(Record.Level);

        if (bUseColor)
        {
            Batch.Append(Descriptor.AnsiColor.data(), static_cast<uint32>(Descriptor.AnsiColor.size()));
        }

        Batch.AppendChar('[');
        Logging::AppendClock(Batch, *Record.Timestamp);
        Batch.AppendLiteral("] Lumina: ");
        Batch.Append(Record.Message);

        if (bUseColor)
        {
            Batch.AppendLiteral("\x1b[m");
        }

        Batch.AppendChar('\n');
    }

    void FStdoutSink::Flush()
    {
        if (Batch.IsEmpty())
        {
            return;
        }

        std::fwrite(Batch.Data(), 1, Batch.Size(), stdout);
        std::fflush(stdout);
        Batch.Clear();
    }
}
