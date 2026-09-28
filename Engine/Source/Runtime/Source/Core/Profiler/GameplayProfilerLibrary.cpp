#include "RuntimePCH.h"

#include "GameplayProfilerLibrary.h"

#include "Core/Profiler/GameplayProfiler.h"
#include "tracy/TracyC.h"

namespace Lumina
{
    namespace
    {
        #if defined(TRACY_ENABLE)
        // One per open script sample, kept even when inactive so every EndScope pops exactly what its BeginScope pushed.
        thread_local TVector<TracyCZoneCtx> GScriptZones;
        #endif
    }

    void CGameplayProfilerLibrary::BeginScope(const FString& Name)
    {
        #if defined(TRACY_ENABLE)
        // Also a Tracy zone named at runtime, so script work sampled by name reads by that name in a capture.
        TracyCZoneCtx Zone{};
        if (!Name.empty() && TracyCIsConnected)
        {
            const uint64 SourceLocation = ___tracy_alloc_srcloc_name(__LINE__, __FILE__, strlen(__FILE__), __FUNCTION__, strlen(__FUNCTION__),
                                                                     Name.c_str(), Name.size(), 0);
            Zone = ___tracy_emit_zone_begin_alloc(SourceLocation, 1);
        }
        GScriptZones.push_back(Zone);
        #endif

        if (!Name.empty())
        {
            FGameplayProfiler::Get().BeginScope(FStringView(Name.c_str(), Name.size()));
        }
    }

    void CGameplayProfilerLibrary::EndScope()
    {
        FGameplayProfiler::Get().EndScope();

        #if defined(TRACY_ENABLE)
        if (!GScriptZones.empty())
        {
            const TracyCZoneCtx Zone = GScriptZones.back();
            GScriptZones.pop_back();
            if (Zone.active)
            {
                ___tracy_emit_zone_end(Zone);
            }
        }
        #endif
    }

    bool CGameplayProfilerLibrary::IsProfilerEnabled()
    {
        return FGameplayProfiler::Get().IsEnabled();
    }
}
