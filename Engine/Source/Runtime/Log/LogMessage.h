#pragma once
#include "Containers/String.h"
#include "LogLevel.h"


namespace Lumina
{
    struct FConsoleMessage
    {
        FFixedString        Message;
        TFixedString<24>    Time;
        FStringView         LoggerName;
        ELogLevel           Level = ELogLevel::Info;
    };
}
