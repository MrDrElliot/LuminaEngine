#pragma once

#include "Containers/StringView.h"
#include "Log/Log.h"

namespace Lumina
{
    // Flushes the async log backend, then counts console messages at Level that contain every needle.
    template<typename... TNeedles>
    int32 CountLoggedMessages(ELogLevel Level, TNeedles&&... Needles)
    {
        Logging::Flush();
        const Logging::FLogQueue& Messages = Logging::GetConsoleLogQueue();
        int32 Count = 0;
        for (int32 Index = 0; Index < Messages.Num(); ++Index)
        {
            const FStringView Text = Messages[Index].Message.View();
            if (Messages[Index].Level == Level && (Text.Contains(Needles) && ...))
            {
                ++Count;
            }
        }
        return Count;
    }
}
