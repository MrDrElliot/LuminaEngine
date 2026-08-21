#include "MaterialNode_VectorOps.h"

#include "Core/Object/Cast.h"
#include "UI/Tools/NodeGraph/Material/MaterialOutput.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"

namespace Lumina
{
    void CMaterialExpression_ComponentMask::BuildNode()
    {
        Super::BuildNode();

        InputPin = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "Input", ENodePinDirection::Input));
        InputPin->SetInputType(EMaterialInputType::Float4);
        InputPin->SetComponentMask(EComponentMask::RGBA);
    }

    FStringView CMaterialExpression_ComponentMask::GetNodeDisplayName() const
    {
        // Rebuilt into an inline-storage member so the returned view stays valid after this returns.
        CachedDisplayName = "ComponentMask_";
        if (R) CachedDisplayName.append("R");
        if (G) CachedDisplayName.append("G");
        if (B) CachedDisplayName.append("B");
        if (A) CachedDisplayName.append("A");
        return FStringView(CachedDisplayName.c_str(), CachedDisplayName.size());
    }

    void CMaterialExpression_ComponentMask::GenerateDefinition(FMaterialCompiler& Compiler) { Compiler.ComponentMask(InputPin); }

    ImVec2 CMaterialExpression_ComponentMask::GetMinNodeTitleBarSize() const
    {
        return ImVec2(22.0f, Super::GetMinNodeTitleBarSize().y);
    }

    void CMaterialExpression_Append::BuildNode()
    {
        Super::BuildNode();

        InputA = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "A", ENodePinDirection::Input));
        InputA->SetInputType(EMaterialInputType::Float4);
        InputA->SetComponentMask(EComponentMask::RGBA);

        InputB = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "B", ENodePinDirection::Input));
        InputB->SetInputType(EMaterialInputType::Float4);
        InputB->SetComponentMask(EComponentMask::RGBA);

        Output->SetComponentMask(EComponentMask::RGBA);
    }
    void CMaterialExpression_Append::GenerateDefinition(FMaterialCompiler& Compiler) { Compiler.Append(InputA, InputB); }

    void CMaterialExpression_MakeFloat2::BuildNode()
    {
        Super::BuildNode();
        Output->SetInputType(EMaterialInputType::Float2);
        Output->SetComponentMask(EComponentMask::RG);

        R = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "R", ENodePinDirection::Input));
        R->SetInputType(EMaterialInputType::Float); R->SetComponentMask(EComponentMask::R);
        G = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "G", ENodePinDirection::Input));
        G->SetInputType(EMaterialInputType::Float); G->SetComponentMask(EComponentMask::G);
    }
    void CMaterialExpression_MakeFloat2::GenerateDefinition(FMaterialCompiler& Compiler) { Compiler.MakeFloat2(R, G); }

    void CMaterialExpression_MakeFloat3::BuildNode()
    {
        Super::BuildNode();
        Output->SetInputType(EMaterialInputType::Float3);
        Output->SetComponentMask(EComponentMask::RGB);

        R = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "R", ENodePinDirection::Input));
        R->SetInputType(EMaterialInputType::Float); R->SetComponentMask(EComponentMask::R);
        G = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "G", ENodePinDirection::Input));
        G->SetInputType(EMaterialInputType::Float); G->SetComponentMask(EComponentMask::G);
        B = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "B", ENodePinDirection::Input));
        B->SetInputType(EMaterialInputType::Float); B->SetComponentMask(EComponentMask::B);
    }
    void CMaterialExpression_MakeFloat3::GenerateDefinition(FMaterialCompiler& Compiler) { Compiler.MakeFloat3(R, G, B); }

    void CMaterialExpression_MakeFloat4::BuildNode()
    {
        Super::BuildNode();
        Output->SetInputType(EMaterialInputType::Float4);
        Output->SetComponentMask(EComponentMask::RGBA);

        R = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "R", ENodePinDirection::Input));
        R->SetInputType(EMaterialInputType::Float); R->SetComponentMask(EComponentMask::R);
        G = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "G", ENodePinDirection::Input));
        G->SetInputType(EMaterialInputType::Float); G->SetComponentMask(EComponentMask::G);
        B = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "B", ENodePinDirection::Input));
        B->SetInputType(EMaterialInputType::Float); B->SetComponentMask(EComponentMask::B);
        A = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "A", ENodePinDirection::Input));
        A->SetInputType(EMaterialInputType::Float); A->SetComponentMask(EComponentMask::A);
    }
    void CMaterialExpression_MakeFloat4::GenerateDefinition(FMaterialCompiler& Compiler) { Compiler.MakeFloat4(R, G, B, A); }

    void CMaterialExpression_BreakFloat2::BuildNode()
    {
        InputPin = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "Vec2", ENodePinDirection::Input));
        InputPin->SetPinName("Vec2"); InputPin->SetShouldDrawEditor(true); InputPin->SetIndex(0);

        R = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "R", ENodePinDirection::Output));
        R->SetShouldDrawEditor(true); R->SetHideDuringConnection(false); R->SetPinName("R");
        R->SetPinColor(IM_COL32(255, 10, 10, 255)); R->SetComponentMask(EComponentMask::R);
        R->InputType = EMaterialInputType::Float;

        G = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "G", ENodePinDirection::Output));
        G->SetShouldDrawEditor(true); G->SetHideDuringConnection(false); G->SetPinName("G");
        G->SetPinColor(IM_COL32(10, 255, 10, 255)); G->SetComponentMask(EComponentMask::G);
        G->InputType = EMaterialInputType::Float;
    }
    void CMaterialExpression_BreakFloat2::GenerateDefinition(FMaterialCompiler& Compiler) { Compiler.BreakFloat2(InputPin); }

    void CMaterialExpression_BreakFloat3::BuildNode()
    {
        InputPin = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "Vec3", ENodePinDirection::Input));
        InputPin->SetPinName("Vec3"); InputPin->SetShouldDrawEditor(true); InputPin->SetIndex(0);

        R = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "R", ENodePinDirection::Output));
        R->SetShouldDrawEditor(true); R->SetHideDuringConnection(false); R->SetPinName("R");
        R->SetPinColor(IM_COL32(255, 10, 10, 255)); R->SetComponentMask(EComponentMask::R);
        R->InputType = EMaterialInputType::Float;

        G = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "G", ENodePinDirection::Output));
        G->SetShouldDrawEditor(true); G->SetHideDuringConnection(false); G->SetPinName("G");
        G->SetPinColor(IM_COL32(10, 255, 10, 255)); G->SetComponentMask(EComponentMask::G);
        G->InputType = EMaterialInputType::Float;

        B = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "B", ENodePinDirection::Output));
        B->SetShouldDrawEditor(true); B->SetHideDuringConnection(false); B->SetPinName("B");
        B->SetPinColor(IM_COL32(10, 10, 255, 255)); B->SetComponentMask(EComponentMask::B);
        B->InputType = EMaterialInputType::Float;
    }
    void CMaterialExpression_BreakFloat3::GenerateDefinition(FMaterialCompiler& Compiler) { Compiler.BreakFloat3(InputPin); }

    void CMaterialExpression_BreakFloat4::BuildNode()
    {
        InputPin = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "Vec4", ENodePinDirection::Input));
        InputPin->SetPinName("Vec4"); InputPin->SetShouldDrawEditor(true); InputPin->SetIndex(0);

        R = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "R", ENodePinDirection::Output));
        R->SetShouldDrawEditor(true); R->SetHideDuringConnection(false); R->SetPinName("R");
        R->SetPinColor(IM_COL32(255, 10, 10, 255)); R->SetComponentMask(EComponentMask::R);
        R->InputType = EMaterialInputType::Float;

        G = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "G", ENodePinDirection::Output));
        G->SetShouldDrawEditor(true); G->SetHideDuringConnection(false); G->SetPinName("G");
        G->SetPinColor(IM_COL32(10, 255, 10, 255)); G->SetComponentMask(EComponentMask::G);
        G->InputType = EMaterialInputType::Float;

        B = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "B", ENodePinDirection::Output));
        B->SetShouldDrawEditor(true); B->SetHideDuringConnection(false); B->SetPinName("B");
        B->SetPinColor(IM_COL32(10, 10, 255, 255)); B->SetComponentMask(EComponentMask::B);
        B->InputType = EMaterialInputType::Float;

        A = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "A", ENodePinDirection::Output));
        A->SetShouldDrawEditor(true); A->SetHideDuringConnection(false); A->SetPinName("A");
        A->SetComponentMask(EComponentMask::A); A->InputType = EMaterialInputType::Float;
    }
    void CMaterialExpression_BreakFloat4::GenerateDefinition(FMaterialCompiler& Compiler) { Compiler.BreakFloat4(InputPin); }

    static void BuildSingleInput(CMaterialExpression_Math* Self)
    {
        Self->A = Cast<CMaterialInput>(Self->CreatePin(CMaterialInput::StaticClass(), "X", ENodePinDirection::Input));
        Self->A->SetPinName("X"); Self->A->SetShouldDrawEditor(true); Self->A->SetIndex(0);
    }

    static void BuildBinaryInputs(CMaterialExpression_Math* Self)
    {
        Self->A = Cast<CMaterialInput>(Self->CreatePin(CMaterialInput::StaticClass(), "X", ENodePinDirection::Input));
        Self->A->SetPinName("X"); Self->A->SetShouldDrawEditor(true); Self->A->SetIndex(0);
        Self->B = Cast<CMaterialInput>(Self->CreatePin(CMaterialInput::StaticClass(), "Y", ENodePinDirection::Input));
        Self->B->SetPinName("Y"); Self->B->SetShouldDrawEditor(true); Self->B->SetIndex(1);
    }

    void CMaterialExpression_Normalize::BuildNode() { Super::BuildNode(); BuildSingleInput(this); }
    void CMaterialExpression_Normalize::GenerateDefinition(FMaterialCompiler& Compiler) { Compiler.Normalize(A); }

    void CMaterialExpression_Distance::BuildNode() { Super::BuildNode(); BuildBinaryInputs(this); }
    void CMaterialExpression_Distance::GenerateDefinition(FMaterialCompiler& Compiler) { Compiler.Distance(A, B); }

    void CMaterialExpression_Length::BuildNode() { Super::BuildNode(); BuildSingleInput(this); }
    void CMaterialExpression_Length::GenerateDefinition(FMaterialCompiler& Compiler) { Compiler.Length(A); }

    void CMaterialExpression_Dot::BuildNode() { Super::BuildNode(); BuildBinaryInputs(this); }
    void CMaterialExpression_Dot::GenerateDefinition(FMaterialCompiler& Compiler) { Compiler.Dot(A, B); }

    void CMaterialExpression_Cross::BuildNode()
    {
        Super::BuildNode(); 
        BuildBinaryInputs(this);
    }
    
    void CMaterialExpression_Cross::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        Compiler.Cross(A, B);
    }

    void CMaterialExpression_Reflect::BuildNode()
    {
        Super::BuildNode();
        I = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "I", ENodePinDirection::Input));
        N = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "N", ENodePinDirection::Input));
    }
    
    void CMaterialExpression_Reflect::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        Compiler.Reflect(I, N);
    }

    void CMaterialExpression_Refract::BuildNode()
    {
        Super::BuildNode();
        I = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "I", ENodePinDirection::Input));
        N = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "N", ENodePinDirection::Input));
        Eta = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "Eta", ENodePinDirection::Input));
    }
    
    void CMaterialExpression_Refract::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        Compiler.Refract(I, N, Eta);
    }

    void CMaterialExpression_RotateAboutAxis::BuildNode()
    {
        Super::BuildNode();
        Position = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "Position", ENodePinDirection::Input));
        Axis     = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "Axis", ENodePinDirection::Input));
        Angle    = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "Angle", ENodePinDirection::Input));
        Pivot    = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "Pivot", ENodePinDirection::Input));
    }
    
    void CMaterialExpression_RotateAboutAxis::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        Compiler.RotateAboutAxis(Position, Axis, Angle, Pivot);
    }
}
