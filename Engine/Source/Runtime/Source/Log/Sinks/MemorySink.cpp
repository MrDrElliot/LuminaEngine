#include "RuntimePCH.h"
#include "MemorySink.h"

#include "Core/Templates/LuminaTemplate.h"

namespace Lumina
{
    void FMemorySink::Write(const Logging::FLogRecord& Record)
    {
        FConsoleMessage Message;
        Message.Time.assign(Record.Timestamp->Clock, Record.Timestamp->Clock + 8);
        Message.LoggerName = "Lumina";
        Message.Message.assign(Record.Message.data(), Record.Message.data() + Record.Message.size());
        Message.Level      = Record.Level;

        OutputMessages.push_back(Move(Message));
    }
}
