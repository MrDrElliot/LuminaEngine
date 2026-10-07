#ifdef __linux__

#include <csignal>

#include "CpuCheck.h"

extern int LuminaMain(int ArgC, char** ArgV);

namespace
{
    void IgnoreBrokenPipeSignal()
    {
        struct sigaction Action = {};
        Action.sa_handler = SIG_IGN;
        sigemptyset(&Action.sa_mask);

        ::sigaction(SIGPIPE, &Action, nullptr);
    }
}

int main(int ArgC, char** ArgV)
{
    if (!LuminaHasRequiredCpu())
    {
        LuminaReportUnsupportedCpu();
        return 1;
    }

    IgnoreBrokenPipeSignal();

    return LuminaMain(ArgC, ArgV);
}

#endif
