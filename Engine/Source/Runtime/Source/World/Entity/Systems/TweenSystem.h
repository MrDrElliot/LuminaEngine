#pragma once
#include "EntitySystem.h"
#include "TweenSystem.generated.h"

namespace Lumina
{
    // Serial by default, since a tween setter or its finished callback can touch anything.
    REFLECT()
    class STweenSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnUpdate() override;
        bool HasWork(EUpdateStage Stage) override;
    };
}
