#pragma once
#include "EntitySystem.h"
#include "World/Entity/Components/InputComponent.h"
#include "InputSystem.generated.h"

namespace Lumina
{
    // Listener callbacks run script, so dispatch waits for the flush on the main thread.
    REFLECT()
    class CInputSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnUpdate(const SSystemContext& Context) override;

    private:

        void Dispatch(const SExclusiveContext& Context);

        ECS::TQuery<ECS::Read<SInputComponent>> Listeners;
        TVector<int32> ChangedActionIndices;
    };
}
