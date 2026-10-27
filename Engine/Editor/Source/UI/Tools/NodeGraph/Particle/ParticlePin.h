#pragma once

#include "UI/Tools/NodeGraph/EdNodeGraphPin.h"
#include "ParticlePin.generated.h"

namespace Lumina
{
    enum class EParticlePinType : uint8
    {
        Float,
        Float2,
        Float3,
        Float4,
    };

    REFLECT()
    class CParticleInput : public CEdNodeGraphPin
    {
        GENERATED_BODY()
    public:

        float DrawPin() override;

        void SetPinType(EParticlePinType InType) { PinType = InType; }
        EParticlePinType GetPinType() const { return PinType; }

        // The value an unwired pin starts at, until the node has saved one of its own.
        void SetDefaultFloat(float V)              { InitialDefault = FVector4(V, 0.0f, 0.0f, 0.0f); }
        void SetDefaultFloat3(const FVector3& V)   { InitialDefault = FVector4(V, 0.0f); }
        void SetDefaultFloat4(const FVector4& V)   { InitialDefault = V; }

        // What the pin reads while unwired, kept on the owning node since pins are rebuilt on load.
        FVector4& GetDefault();

    private:

        EParticlePinType PinType = EParticlePinType::Float;
        FVector4         InitialDefault = FVector4(0.0f);
    };

    REFLECT()
    class CParticleOutput : public CEdNodeGraphPin
    {
        GENERATED_BODY()
    public:

        float DrawPin() override;

        void SetPinType(EParticlePinType InType) { PinType = InType; }
        EParticlePinType GetPinType() const { return PinType; }

        // The variable a compile bound this output to, for a node with more than one, or empty to read the node's own name.
        FString ResolvedVar;

    private:

        EParticlePinType PinType = EParticlePinType::Float;
    };
}
