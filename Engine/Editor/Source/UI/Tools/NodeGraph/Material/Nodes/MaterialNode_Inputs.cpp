#include "MaterialNode_Inputs.h"

#include "Core/Object/Cast.h"
#include "UI/Tools/NodeGraph/Material/MaterialOutput.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"

namespace Lumina
{
    void CMaterialExpression_Panner::BuildNode()
    {
        Super::BuildNode();

        UV = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "UV", ENodePinDirection::Input));
        UV->SetPinColor(IM_COL32(255, 10, 10, 255));
        UV->SetHideDuringConnection(false);
        UV->SetPinName("UV");
        UV->SetInputType(EMaterialInputType::Float2);

        Time = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "Time", ENodePinDirection::Input));
        Time->SetHideDuringConnection(false);
        Time->SetPinName("Time");
        Time->SetInputType(EMaterialInputType::Float);

        Speed = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "Speed", ENodePinDirection::Input));
        Speed->SetHideDuringConnection(false);
        Speed->SetPinName("Speed");
        Speed->SetInputType(EMaterialInputType::Float2);

        Output->SetInputType(EMaterialInputType::Float2);
    }
    
    void CMaterialExpression_Panner::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        Compiler.Panner(UV, Time, Speed);
    }

    void CMaterialExpression_TexCoords::BuildNode()
    {
        CMaterialExpression::BuildNode();

        Tiling = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "Tiling", ENodePinDirection::Input));
        Tiling->SetHideDuringConnection(false);
        Tiling->SetPinName("Tiling");
        Tiling->SetInputType(EMaterialInputType::Float2);

        Rotation = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "Rotation", ENodePinDirection::Input));
        Rotation->SetHideDuringConnection(false);
        Rotation->SetPinName("Rotation");
        Rotation->SetInputType(EMaterialInputType::Float);

        Output->SetInputType(EMaterialInputType::Float2);
        Output->SetComponentMask(EComponentMask::RG);
    }

    void CMaterialExpression_TexCoords::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        Compiler.TexCoords(FullName, TextureIndex, Tiling, UTiling, VTiling, Rotation, RotationDegrees);
    }

    void CMaterialExpression_WorldPos::BuildNode()
    {
        Super::BuildNode();
        Output->SetInputType(EMaterialInputType::Float3);
        Output->SetComponentMask(EComponentMask::RGB);
    }
    void CMaterialExpression_WorldPos::GenerateDefinition(FMaterialCompiler& Compiler) { Compiler.WorldPos(FullName, this); }

    void CMaterialExpression_CameraPos::BuildNode()
    {
        Super::BuildNode();
        Output->SetInputType(EMaterialInputType::Float3);
        Output->SetComponentMask(EComponentMask::RGB);
    }
    void CMaterialExpression_CameraPos::GenerateDefinition(FMaterialCompiler& Compiler) { Compiler.CameraPos(FullName, this); }

    void CMaterialExpression_ObjectScale::BuildNode()
    {
        Super::BuildNode();
        Output->SetInputType(EMaterialInputType::Float3);
        Output->SetComponentMask(EComponentMask::RGB);
    }
    void CMaterialExpression_ObjectScale::GenerateDefinition(FMaterialCompiler& Compiler) { Compiler.ObjectScale(FullName, this); }

    void CMaterialExpression_ObjectPosition::BuildNode()
    {
        Super::BuildNode();
        Output->SetInputType(EMaterialInputType::Float3);
        Output->SetComponentMask(EComponentMask::RGB);
    }
    void CMaterialExpression_ObjectPosition::GenerateDefinition(FMaterialCompiler& Compiler) { Compiler.ObjectPosition(FullName, this); }

    void CMaterialExpression_EntityID::BuildNode()
    {
        Super::BuildNode();
        Output->SetInputType(EMaterialInputType::Float);
    }
    void CMaterialExpression_EntityID::GenerateDefinition(FMaterialCompiler& Compiler) { Compiler.EntityID(FullName); }

    void CMaterialExpression_VertexNormal::BuildNode()
    {
        Super::BuildNode();
        Output->SetInputType(EMaterialInputType::Float3);
        Output->SetComponentMask(EComponentMask::RGB);
    }
    void CMaterialExpression_VertexNormal::GenerateDefinition(FMaterialCompiler& Compiler) { Compiler.VertexNormal(FullName, this); }

    void CMaterialExpression_VertexTangent::BuildNode()
    {
        Super::BuildNode();
        Output->SetInputType(EMaterialInputType::Float3);
        Output->SetComponentMask(EComponentMask::RGB);
    }
    void CMaterialExpression_VertexTangent::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        if (!Compiler.RequirePixelStage(this, "VertexTangent")) return;
        Compiler.VertexTangent(FullName, this);
    }

    void CMaterialExpression_VertexBitangent::BuildNode()
    {
        Super::BuildNode();
        Output->SetInputType(EMaterialInputType::Float3);
        Output->SetComponentMask(EComponentMask::RGB);
    }
    void CMaterialExpression_VertexBitangent::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        if (!Compiler.RequirePixelStage(this, "VertexBitangent")) return;
        Compiler.VertexBitangent(FullName, this);
    }

    void CMaterialExpression_VertexColor::BuildNode()
    {
        Super::BuildNode();
        Output->SetInputType(EMaterialInputType::Float4);
        Output->SetComponentMask(EComponentMask::RGBA);
    }
    void CMaterialExpression_VertexColor::GenerateDefinition(FMaterialCompiler& Compiler) { Compiler.VertexColor(FullName, this); }
}
