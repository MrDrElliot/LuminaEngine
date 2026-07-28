#pragma once
#include "Containers/String.h"


namespace spdlog::level
{
    // Opaque enum declaration. Because the underlying type is fixed, the type is complete from here,
    // so it can be used as a member and cast to - but its enumerators are not visible without
    // <spdlog/common.h>, which is exactly what we're keeping out of this header.
    enum level_enum : int;
}

namespace Lumina
{
    // spdlog::level::info. Spelling the enumerator here would require including spdlog, which would
    // put it back in the ~380 TUs that reach Log.h. Log.cpp static_asserts this stays in sync.
    inline constexpr int GDefaultConsoleMessageLevel = 2;

    struct FConsoleMessage
    {
        FFixedString        Message;
        TFixedString<24>    Time;
        FStringView         LoggerName;

		spdlog::level::level_enum Level =
		    static_cast<spdlog::level::level_enum>(GDefaultConsoleMessageLevel);
    };
}
