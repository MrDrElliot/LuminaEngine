#pragma once
#include "MaterialNodeExpression.h"
#include "MaterialNode_Particle.generated.h"

namespace Lumina
{
    // Per-sprite values the particle simulation writes, readable only from a Particle material.

    REFLECT(NotPlaceable)
    class CMaterialExpression_ParticleInput : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        uint32 GetNodeTitleColor() const override { return IM_COL32(190, 95, 35, 255); }
        FFixedString GetNodeCategory() const override { return "Particles"; }
    };

    REFLECT()
    class CMaterialExpression_ParticleColor : public CMaterialExpression_ParticleInput
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FStringView GetNodeDisplayName() const override { return "ParticleColor"; }
        FStringView GetNodeTooltip() const override { return "The simulated per-particle color (float4). Wire it into Emissive to keep the colors the emitter's modules produce."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
    };

    REFLECT()
    class CMaterialExpression_ParticlePosition : public CMaterialExpression_ParticleInput
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FStringView GetNodeDisplayName() const override { return "ParticlePosition"; }
        FStringView GetNodeTooltip() const override { return "World-space position of the particle itself (float3), which is the center of its quad rather than the shaded corner."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
    };

    REFLECT()
    class CMaterialExpression_ParticleVelocity : public CMaterialExpression_ParticleInput
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FStringView GetNodeDisplayName() const override { return "ParticleVelocity"; }
        FStringView GetNodeTooltip() const override { return "World-space velocity of the particle (float3), in units per second."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
    };

    REFLECT()
    class CMaterialExpression_ParticleDirection : public CMaterialExpression_ParticleInput
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FStringView GetNodeDisplayName() const override { return "ParticleDirection"; }
        FStringView GetNodeTooltip() const override { return "Normalized direction of travel (float3). A particle that has not moved reads as world up."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
    };

    REFLECT()
    class CMaterialExpression_ParticleSpeed : public CMaterialExpression_ParticleInput
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FStringView GetNodeDisplayName() const override { return "ParticleSpeed"; }
        FStringView GetNodeTooltip() const override { return "Length of the particle's velocity (float), in units per second."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
    };

    REFLECT()
    class CMaterialExpression_ParticleSize : public CMaterialExpression_ParticleInput
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FStringView GetNodeDisplayName() const override { return "ParticleSize"; }
        FStringView GetNodeTooltip() const override { return "Half-extent of the sprite quad along its own right and up axes (float2), after any size-scale attributes."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
    };

    REFLECT()
    class CMaterialExpression_ParticleRelativeTime : public CMaterialExpression_ParticleInput
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FStringView GetNodeDisplayName() const override { return "ParticleRelativeTime"; }
        FStringView GetNodeTooltip() const override { return "Normalized age from 0 at spawn to 1 at death (float). The usual driver for fade and flipbook curves."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
    };

    REFLECT()
    class CMaterialExpression_ParticleRandom : public CMaterialExpression_ParticleInput
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FStringView GetNodeDisplayName() const override { return "ParticleRandom"; }
        FStringView GetNodeTooltip() const override { return "Stable 0..1 value derived from the particle's spawn seed (float), for varying one sprite against its neighbors."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
    };
}
