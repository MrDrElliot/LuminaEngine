#pragma once
#include "ExclusiveSystem.h"
#include "World/Entity/Components/InputComponent.h"
#include "InputSystem.generated.h"

namespace Lumina
{

    REFLECT()
    class SInputSystem : public CExclusiveSystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnExclusiveUpdate(const SExclusiveContext& Context) override;

    private:

        ECS::TQuery<ECS::Read<SInputComponent>> Listeners;
        TVector<int32> ChangedActionIndices;
    };
}
