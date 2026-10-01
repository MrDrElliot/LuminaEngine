#pragma once

#include "tracy/Tracy.hpp"


#define LUMINA_PROFILE_SCOPE()                      ZoneScoped
#define LUMINA_PROFILE_SCOPE_COLORED(c)             ZoneScopedC(c)
#define LUMINA_PROFILE_FRAME(x)                     FrameMark
#define LUMINA_PROFILE_SECTION(x)                   ZoneScopedN(x)
#define LUMINA_PROFILE_SECTION_COLORED(x, c)        ZoneScopedNC(x, c)
// Caller attribution costs a stack walk per hit, so it is opt-in at the few sites that need it.
#define LUMINA_PROFILE_SCOPE_CALLSTACK(Depth)       ZoneScopedS(Depth)
#define LUMINA_PROFILE_SECTION_CALLSTACK(x, Depth)  ZoneScopedNS(x, Depth)

#define LUMINA_PROFILE_TAG(x)                       ZoneText(x, strlen(x))
// Renames the open zone, so a dispatcher's bar reads as the work rather than as the dispatcher.
#define LUMINA_PROFILE_NAME(x)                      ZoneName(x, strlen(x))
// A zone named at runtime, which unlike a renamed zone keeps that name in a CSV export. Once-per-item work only.
#define LUMINA_PROFILE_SECTION_DYNAMIC(x)           ZoneTransientN(LuminaDynamicZone, x, true)

#if defined(TRACY_ENABLE)
namespace Lumina::Profiler
{
    // Where a zone named by x records itself, built once per thread and name, so x must outlive the process.
    RUNTIME_API const tracy::SourceLocationData* NamedLocation(const char* Name);
}
// A runtime-named zone as cheap as a static one, for per-job and per-system names that are pooled or literal.
#define LUMINA_PROFILE_SECTION_NAMED(x)             tracy::ScopedZone LuminaNamedZone(::Lumina::Profiler::NamedLocation(x), -1, true)
#else
#define LUMINA_PROFILE_SECTION_NAMED(x)
#endif
#define LUMINA_PROFILE_LOG(text, size)              TracyMessage(text, size)
#define LUMINA_PROFILE_VALUE(text, value)           TracyPlot(text, value)
