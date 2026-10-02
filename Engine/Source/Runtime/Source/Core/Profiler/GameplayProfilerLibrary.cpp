#include "RuntimePCH.h"

#include "GameplayProfilerLibrary.h"

#include "Core/Profiler/GameplayProfiler.h"
#include "tracy/TracyC.h"
#include "Containers/HashTable.h"
#include "Core/Threading/Sync.h"
#include <atomic>

namespace Lumina
{
    namespace
    {
        #if defined(TRACY_ENABLE)
        // One per open script sample, kept even when inactive so every EndScope pops exactly what its BeginScope pushed.
        thread_local TVector<TracyCZoneCtx> GScriptZones;
        #endif

        struct FRegisteredScope
        {
            FString Name;
            #if defined(TRACY_ENABLE)
            ___tracy_source_location_data SourceLocation{};
            #endif
        };

        // Entries never move or die, since Tracy keeps pointers into a source location for the whole capture.
        constexpr int32 kMaxRegisteredScopes = 4096;
        FRegisteredScope GRegisteredScopes[kMaxRegisteredScopes];
        std::atomic<int32> GRegisteredScopeCount{0};
        FMutex GRegisterMutex;
        THashMap<FString, int32> GScopeIdsByName;
    }

    int32 CGameplayProfilerLibrary::RegisterScope(const FString& Name)
    {
        FScopeLock Lock(GRegisterMutex);
        if (auto It = GScopeIdsByName.find(Name); It != GScopeIdsByName.end())
        {
            return It->second;
        }

        const int32 Id = GRegisteredScopeCount.load(std::memory_order_relaxed);
        if (Id >= kMaxRegisteredScopes)
        {
            return -1;
        }

        FRegisteredScope& Scope = GRegisteredScopes[Id];
        Scope.Name = Name;
        #if defined(TRACY_ENABLE)
        Scope.SourceLocation.name     = Scope.Name.c_str();
        Scope.SourceLocation.function = "Script";
        Scope.SourceLocation.file     = __FILE__;
        Scope.SourceLocation.line     = __LINE__;
        #endif
        GScopeIdsByName.emplace(Name, Id);
        GRegisteredScopeCount.store(Id + 1, std::memory_order_release);
        return Id;
    }

    void CGameplayProfilerLibrary::BeginRegisteredScope(int32 ScopeId)
    {
        if (ScopeId < 0 || ScopeId >= GRegisteredScopeCount.load(std::memory_order_acquire))
        {
            // Pushed anyway, so the paired EndScope still pops what this pushed.
            #if defined(TRACY_ENABLE)
            GScriptZones.push_back(TracyCZoneCtx{});
            #endif
            return;
        }

        const FRegisteredScope& Scope = GRegisteredScopes[ScopeId];
        #if defined(TRACY_ENABLE)
        GScriptZones.push_back(TracyCIsConnected ? ___tracy_emit_zone_begin(&Scope.SourceLocation, 1) : TracyCZoneCtx{});
        #endif

        if (FGameplayProfiler::Get().IsEnabled())
        {
            FGameplayProfiler::Get().BeginScope(FStringView(Scope.Name.c_str(), Scope.Name.size()));
        }
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
