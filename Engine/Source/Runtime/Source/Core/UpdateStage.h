#pragma once

#include "Memory/Memory.h"
#include "Platform/GenericPlatform.h"
#include "Object/ObjectMacros.h"
#include "UpdateStage.generated.h"

namespace Lumina
{
    REFLECT()
    enum class EUpdateStage : uint8
    {
        FrameStart,
        PrePhysics,
        DuringPhysics,
        PostPhysics,
        FrameEnd,
        Paused,
        Max,
    };
    
    struct FUpdateStage_FrameStart      {};
    struct FUpdateStage_PrePhysics      {};
    struct FUpdateStage_DuringPhysics   {};
    struct FUpdateStage_PostPhysics     {};
    struct FUpdateStage_FrameEnd        {};
    struct FUpdateStage_Paused          {};
    
    constexpr const char* GUpdateStageNames[] = 
    {
        "FrameStart",
        "PrePhysics",
        "DuringPhysics",
        "PostPhysics",
        "FrameEnd",
        "Paused"
    };

    #define US_FrameStart       EUpdateStage::FrameStart
    #define US_PrePhysics       EUpdateStage::PrePhysics
    #define US_DuringPhysics    EUpdateStage::DuringPhysics
    #define US_PostPhysics      EUpdateStage::PostPhysics
    #define US_FrameEnd         EUpdateStage::FrameEnd
    #define US_Paused           EUpdateStage::Paused

    // Lower value = higher priority; systems sort ascending so Highest ticks first.
    // Disabled drops the system from the stage entirely.
    enum class EUpdatePriority : uint8
    {
        Highest     = 0,
        High        = 64,
        Medium      = 128,
        Low         = 192,
        Disabled    = 255,
        Default     = Medium,
    };

    struct FUpdateStagePriority
    {
        FUpdateStagePriority(EUpdateStage InStage) : Stage(InStage) { }
        FUpdateStagePriority(EUpdateStage InStage, EUpdatePriority InPriority) : Stage(InStage), Priority(InPriority) { }

    public:

        EUpdateStage     Stage;
        EUpdatePriority  Priority = EUpdatePriority::Default;
    };

    using RequiresUpdate = FUpdateStagePriority;
    
    struct FUpdatePriorityList
    {
        FUpdatePriorityList()
        {
            Reset();
        }

        template<typename... Args>
        requires (sizeof...(Args) > 0) && (eastl::is_constructible_v<FUpdateStagePriority, Args> && ...)
        FUpdatePriorityList(Args&&... args)
        {
            Reset();
            ((*this << eastl::forward<Args>(args)), ...);
        }

        void Reset()
        {
            Memory::Memset(Priorities, (uint8)EUpdatePriority::Disabled, sizeof(Priorities));
        }

        bool IsStageEnabled(EUpdateStage Stage) const
        {
            return Priorities[(uint8)Stage] != (uint8)EUpdatePriority::Disabled;
        }

        uint8 GetPriorityForStage(EUpdateStage Stage) const
        {
            return Priorities[(uint8)Stage];
        }

        FUpdatePriorityList& SetStagePriority(FUpdateStagePriority&& StagePriority)
        {
            Priorities[(uint8) StagePriority.Stage] = (uint8)StagePriority.Priority;
            return *this;
        }

        FUpdatePriorityList& operator<<(FUpdateStagePriority&& StagePriority)
        {
            Priorities[(uint8)StagePriority.Stage] = (uint8)StagePriority.Priority;
            return *this;
        }

        bool AreAllStagesDisabled() const
        {
            const static uint8 DisabledStages[(uint8)EUpdateStage::Max] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
            static_assert(sizeof(DisabledStages) == sizeof(Priorities), "disabled stages must be the same size as the priorities list");
            return memcmp(Priorities, DisabledStages, sizeof(Priorities)) == 0;
        }

    private:

        uint8           Priorities[(uint8)EUpdateStage::Max];
    };
}
