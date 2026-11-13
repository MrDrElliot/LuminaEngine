#pragma once
#include "ExclusiveSystem.h"
#include "Scripting/EntityScript.h"
#include "EntityScriptSystem.generated.h"

namespace Lumina
{
    // Ticks entity scripts in either language, OnReady and OnUpdate in PrePhysics, OnFixedUpdate in EUpdateStage::FixedUpdate.
    REFLECT()
    class RUNTIME_API CEntityScriptSystem : public CExclusiveSystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnExclusiveUpdate(const SExclusiveContext& Context) override;

    private:

        ECS::TQuery<ECS::Read<SEntityScriptComponent>> Scripts;
    };
}
