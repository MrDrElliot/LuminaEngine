#include "RuntimePCH.h"
#include "PluginLoadingPhase.h"

namespace Lumina
{
    FStringView LexToString(EPluginLoadingPhase Phase)
    {
        switch (Phase)
        {
            case EPluginLoadingPhase::Earliest:        return "Earliest";
            case EPluginLoadingPhase::Core:            return "Core";
            case EPluginLoadingPhase::PreEngineInit:   return "PreEngineInit";
            case EPluginLoadingPhase::EngineInit:      return "EngineInit";
            case EPluginLoadingPhase::PostEngineInit:  return "PostEngineInit";
            case EPluginLoadingPhase::PostProjectLoad: return "PostProjectLoad";
            case EPluginLoadingPhase::EditorInit:      return "EditorInit";
            default:                                   return "Unknown";
        }
    }

    bool TryParsePluginLoadingPhase(FStringView Str, EPluginLoadingPhase& OutPhase)
    {
        for (uint8 Index = 0; Index < static_cast<uint8>(EPluginLoadingPhase::Count); ++Index)
        {
            const EPluginLoadingPhase Phase = static_cast<EPluginLoadingPhase>(Index);
            if (Str == LexToString(Phase))
            {
                OutPhase = Phase;
                return true;
            }
        }
        return false;
    }
}
