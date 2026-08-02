#pragma once

#include "tracy/Tracy.hpp"


#define LUMINA_PROFILE_SCOPE()                      ZoneScoped
#define LUMINA_PROFILE_SCOPE_COLORED(c)             ZoneScopedC(c)
#define LUMINA_PROFILE_FRAME(x)                     FrameMark
#define LUMINA_PROFILE_SECTION(x)                   ZoneScopedN(x)
#define LUMINA_PROFILE_SECTION_COLORED(x, c)        ZoneScopedNC(x, c)
#define LUMINA_PROFILE_TAG(x)                       ZoneText(x, strlen(x))
#define LUMINA_PROFILE_LOG(text, size)              TracyMessage(text, size)
#define LUMINA_PROFILE_VALUE(text, value)           TracyPlot(text, value)