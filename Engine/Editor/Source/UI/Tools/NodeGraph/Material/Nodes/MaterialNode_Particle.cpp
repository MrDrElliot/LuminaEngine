#include "MaterialNode_Particle.h"

#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"
#include "UI/Tools/NodeGraph/Material/MaterialOutput.h"

namespace Lumina
{
    void CMaterialExpression_ParticleColor::BuildNode()
    {
        Super::BuildNode();
        Output->SetInputType(EMaterialInputType::Float4);
        Output->SetComponentMask(EComponentMask::RGBA);
    }
    void CMaterialExpression_ParticleColor::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        Compiler.ParticleColor(FullName, this);
    }

    void CMaterialExpression_ParticlePosition::BuildNode()
    {
        Super::BuildNode();
        Output->SetInputType(EMaterialInputType::Float3);
        Output->SetComponentMask(EComponentMask::RGB);
    }
    void CMaterialExpression_ParticlePosition::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        Compiler.ParticlePosition(FullName, this);
    }

    void CMaterialExpression_ParticleVelocity::BuildNode()
    {
        Super::BuildNode();
        Output->SetInputType(EMaterialInputType::Float3);
        Output->SetComponentMask(EComponentMask::RGB);
    }
    void CMaterialExpression_ParticleVelocity::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        Compiler.ParticleVelocity(FullName, this);
    }

    void CMaterialExpression_ParticleDirection::BuildNode()
    {
        Super::BuildNode();
        Output->SetInputType(EMaterialInputType::Float3);
        Output->SetComponentMask(EComponentMask::RGB);
    }
    void CMaterialExpression_ParticleDirection::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        Compiler.ParticleDirection(FullName, this);
    }

    void CMaterialExpression_ParticleSpeed::BuildNode()
    {
        Super::BuildNode();
        Output->SetInputType(EMaterialInputType::Float);
        Output->SetComponentMask(EComponentMask::R);
    }
    void CMaterialExpression_ParticleSpeed::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        Compiler.ParticleSpeed(FullName, this);
    }

    void CMaterialExpression_ParticleSize::BuildNode()
    {
        Super::BuildNode();
        Output->SetInputType(EMaterialInputType::Float2);
        Output->SetComponentMask(EComponentMask::RG);
    }
    void CMaterialExpression_ParticleSize::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        Compiler.ParticleSize(FullName, this);
    }

    void CMaterialExpression_ParticleRelativeTime::BuildNode()
    {
        Super::BuildNode();
        Output->SetInputType(EMaterialInputType::Float);
        Output->SetComponentMask(EComponentMask::R);
    }
    void CMaterialExpression_ParticleRelativeTime::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        Compiler.ParticleRelativeTime(FullName, this);
    }

    void CMaterialExpression_ParticleRandom::BuildNode()
    {
        Super::BuildNode();
        Output->SetInputType(EMaterialInputType::Float);
        Output->SetComponentMask(EComponentMask::R);
    }
    void CMaterialExpression_ParticleRandom::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        Compiler.ParticleRandom(FullName, this);
    }
}
