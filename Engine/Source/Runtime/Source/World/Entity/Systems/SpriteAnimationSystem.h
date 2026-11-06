#pragma once
#include "EntitySystem.h"
#include "Core/Object/ObjectMacros.h"
#include "World/Entity/Components/AnimatedSprite3DComponent.h"
#include "World/Entity/Components/Sprite3DComponent.h"
#include "SpriteAnimationSystem.generated.h"

namespace Lumina
{

    REFLECT()
    class CSpriteAnimationSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnStartup(const SExclusiveContext& Context) override;

        void OnUpdate(const SSystemContext& Context) override;

    private:

        ECS::TQuery<ECS::Write<SAnimatedSprite3DComponent>, ECS::Write<SSprite3DComponent>> Animators;
    };
}
