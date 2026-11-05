#include "RuntimePCH.h"
#include "MemorySink.h"

#include "Core/Templates/LuminaTemplate.h"

namespace Lumina
{
    void FMemorySink::Write(const Logging::FLogRecord& Record)
    {
        FConsoleMessage Message;
        Message.Time = FStringView(Record.Timestamp->Clock, 8);
        Message.LoggerName = "Lumina";
        Message.Message = Record.Message;
        Message.Level      = Record.Level;

        OutputMessages.Add(Move(Message));
    }
}
