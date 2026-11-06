#pragma once
#include "EntitySystem.h"
#include "World/Subsystems/TweenManager.h"
#include "TweenSystem.generated.h"

namespace Lumina
{
    // A tween setter or its finished callback can touch anything, so the tick itself runs at the flush.
    REFLECT()
    class CTweenSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnStartup(const SExclusiveContext& Context) override;
        void OnUpdate(const SSystemContext& Context) override;
        bool HasWork(const SSystemContext& Context) override;

    private:

        TResMut<FTweenManager> Tweens;
    };
}
