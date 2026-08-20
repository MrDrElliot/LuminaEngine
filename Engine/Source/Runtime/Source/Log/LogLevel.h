#pragma once

#include <string_view>

#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"


namespace Lumina
{
    enum class ELogLevel : uint8
    {
        Trace       = 0,
        Debug       = 1,
        Info        = 2,
        Warn        = 3,
        Error       = 4,
        Critical    = 5,
        Off         = 6,
    };

    inline constexpr uint8 GNumLogLevels = static_cast<uint8>(ELogLevel::Off);

    namespace Logging
    {
        // string_view so every length is a compile-time constant.
        struct FLevelDescriptor
        {
            FStringView Name;
            FStringView DisplayName;   // padded to 8 columns
            FStringView AnsiColor;
        };

        inline constexpr FLevelDescriptor GLevelDescriptors[GNumLogLevels + 1] =
        {
            { "trace",    "trace   ", "\x1b[90m"      },
            { "debug",    "debug   ", "\x1b[36m"      },
            { "info",     "info    ", "\x1b[37m"      },
            { "warning",  "warning ", "\x1b[33;1m"    },
            { "error",    "error   ", "\x1b[31;1m"    },
            { "critical", "critical", "\x1b[97;41;1m" },
            { "off",      "off     ", ""              },
        };

        inline constexpr FStringView GAnsiReset = "\x1b[m";

        NODISCARD constexpr const FLevelDescriptor& GetLevelDescriptor(ELogLevel Level) noexcept
        {
            const uint8 Index = static_cast<uint8>(Level);
            return GLevelDescriptors[Index <= GNumLogLevels ? Index : GNumLogLevels];
        }

        NODISCARD constexpr std::string_view ToString(ELogLevel Level) noexcept
        {
            return GetLevelDescriptor(Level).Name;
        }

        NODISCARD constexpr ELogLevel ParseLevel(std::string_view Text, ELogLevel Fallback = ELogLevel::Info) noexcept
        {
            for (uint8 Index = 0; Index <= GNumLogLevels; ++Index)
            {
                if (Text == GLevelDescriptors[Index].Name)
                {
                    return static_cast<ELogLevel>(Index);
                }
            }

            if (Text == "warn")
            {
                return ELogLevel::Warn;
            }
            if (Text == "err")
            {
                return ELogLevel::Error;
            }
            if (Text == "fatal")
            {
                return ELogLevel::Critical;
            }

            return Fallback;
        }
    }
}
