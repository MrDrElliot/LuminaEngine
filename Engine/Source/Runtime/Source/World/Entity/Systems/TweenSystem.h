#pragma once
#include "ExclusiveSystem.h"
#include "World/Subsystems/TweenManager.h"
#include "TweenSystem.generated.h"

namespace Lumina
{
    // Exclusive, since a tween setter or its finished callback can touch anything.
    REFLECT()
    class STweenSystem : public CExclusiveSystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnStartup(const SExclusiveContext& Context) override;
        void OnExclusiveUpdate(const SExclusiveContext& Context) override;
        bool HasWork(const SSystemContext& Context) override;

    private:

        TResMut<FTweenManager> Tweens;
    };
}
