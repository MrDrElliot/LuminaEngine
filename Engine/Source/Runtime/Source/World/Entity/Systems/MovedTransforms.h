#pragma once

#include "Containers/Vector.h"
#include "Platform/GenericPlatform.h"
#include "World/ECS/Entity.h"
#include "World/Entity/Systems/SystemParam.h"

namespace Lumina
{
    enum class EMovedTransforms : uint8
    {
        All,
        Watched,
    };

    // A system member collecting the entities the transform resolve moved since its last drain; it reads STransformComponent, so no transform writer runs beside it.
    class RUNTIME_API FMovedTransforms
    {
    public:

        explicit FMovedTransforms(EMovedTransforms Mode = EMovedTransforms::All);

        FMovedTransforms(const FMovedTransforms&) = delete;
        FMovedTransforms& operator=(const FMovedTransforms&) = delete;

        void Watch(ECS::FEntity Entity, bool bWatch);

        bool Drain(TVector<ECS::FEntity>& Out);

    private:

        void Release();

        ECS::FRegistry*  Registry = nullptr;
        uint32           Consumer = Constants::kIndexNoneU32;
        EMovedTransforms Mode;

        static const FSystemParamOps& GetOps();
    };
}
