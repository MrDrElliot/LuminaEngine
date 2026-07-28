#pragma once
#include "MaterialNodeExpression.h"
#include "MaterialNode_VectorOps.generated.h"

namespace Lumina
{
    REFLECT()
    class CMaterialExpression_ComponentMask : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Vector"; }
        FStringView GetNodeDisplayName() const override;

    private:
        // Backing store for the state-dependent display name above; mutable so the const getter can
        // refresh it. FFixedString = inline storage, so this costs no allocation per frame.
        mutable FFixedString CachedDisplayName;
    public:
        FStringView GetNodeTooltip() const override { return "Selects a subset of the input vector's channels (R/G/B/A)."; }
        void* GetNodeDefaultValue() override { return nullptr; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
        ImVec2 GetMinNodeTitleBarSize() const override;

        CMaterialInput* InputPin = nullptr;

        PROPERTY(Editable) bool R = true;
        PROPERTY(Editable) bool G = true;
        PROPERTY(Editable) bool B = true;
        PROPERTY(Editable) bool A = true;
    };

    REFLECT()
    class CMaterialExpression_Append : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Vector"; }
        FStringView GetNodeDisplayName() const override { return "Append"; }
        FStringView GetNodeTooltip() const override { return "Concatenates the channels of A and B into a wider vector (max 4)."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialInput* InputA = nullptr;
        CMaterialInput* InputB = nullptr;
    };

    REFLECT()
    class CMaterialExpression_MakeFloat2 : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Vector"; }
        FStringView GetNodeDisplayName() const override { return "MakeFloat2"; }
        FStringView GetNodeTooltip() const override { return "Combines two scalars into a float2 (R, G)."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialInput* R = nullptr;
        CMaterialInput* G = nullptr;
    };

    REFLECT()
    class CMaterialExpression_MakeFloat3 : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Vector"; }
        FStringView GetNodeDisplayName() const override { return "MakeFloat3"; }
        FStringView GetNodeTooltip() const override { return "Combines three scalars into a float3 (R, G, B)."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialInput* R = nullptr;
        CMaterialInput* G = nullptr;
        CMaterialInput* B = nullptr;
    };

    REFLECT()
    class CMaterialExpression_MakeFloat4 : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Vector"; }
        FStringView GetNodeDisplayName() const override { return "MakeFloat4"; }
        FStringView GetNodeTooltip() const override { return "Combines four scalars into a float4 (R, G, B, A)."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialInput* R = nullptr;
        CMaterialInput* G = nullptr;
        CMaterialInput* B = nullptr;
        CMaterialInput* A = nullptr;
    };

    REFLECT()
    class CMaterialExpression_BreakFloat2 : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Vector"; }
        FStringView GetNodeDisplayName() const override { return "BreakFloat2"; }
        FStringView GetNodeTooltip() const override { return "Splits a float2 into its R and G scalar components."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialInput* InputPin = nullptr;
        CMaterialOutput* R = nullptr;
        CMaterialOutput* G = nullptr;
    };

    REFLECT()
    class CMaterialExpression_BreakFloat3 : public CMaterialExpression_BreakFloat2
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FStringView GetNodeDisplayName() const override { return "BreakFloat3"; }
        FStringView GetNodeTooltip() const override { return "Splits a float3 into its R, G and B scalar components."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialOutput* B = nullptr;
    };

    REFLECT()
    class CMaterialExpression_BreakFloat4 : public CMaterialExpression_BreakFloat3
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FStringView GetNodeDisplayName() const override { return "BreakFloat4"; }
        FStringView GetNodeTooltip() const override { return "Splits a float4 into its R, G, B and A scalar components."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialOutput* A = nullptr;
    };

    REFLECT()
    class CMaterialExpression_Normalize : public CMaterialExpression_Math
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Vector"; }
        FStringView GetNodeDisplayName() const override { return "Normalize"; }
        FStringView GetNodeTooltip() const override { return "Returns a vector with the same direction as the input and length 1."; }
        void* GetNodeDefaultValue() override { return &ConstA; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
    };

    REFLECT()
    class CMaterialExpression_Distance : public CMaterialExpression_Math
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Vector"; }
        FStringView GetNodeDisplayName() const override { return "Distance"; }
        FStringView GetNodeTooltip() const override { return "Returns the scalar distance between points A and B."; }
        void* GetNodeDefaultValue() override { return &ConstA; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
    };

    REFLECT()
    class CMaterialExpression_Length : public CMaterialExpression_Math
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Vector"; }
        FStringView GetNodeDisplayName() const override { return "Length"; }
        FStringView GetNodeTooltip() const override { return "Returns the length of vector A."; }
        void* GetNodeDefaultValue() override { return &ConstA; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
    };

    REFLECT()
    class CMaterialExpression_Dot : public CMaterialExpression_Math
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Vector"; }
        FStringView GetNodeDisplayName() const override { return "Dot"; }
        FStringView GetNodeTooltip() const override { return "Returns the dot product of A and B (scalar)."; }
        void* GetNodeDefaultValue() override { return &ConstA; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
    };

    REFLECT()
    class CMaterialExpression_Cross : public CMaterialExpression_Math
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Vector"; }
        FStringView GetNodeDisplayName() const override { return "Cross"; }
        FStringView GetNodeTooltip() const override { return "Returns the cross product of A and B (float3)."; }
        void* GetNodeDefaultValue() override { return &ConstA; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;
    };

    REFLECT()
    class CMaterialExpression_Reflect : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Vector"; }
        FStringView GetNodeDisplayName() const override { return "Reflect"; }
        FStringView GetNodeTooltip() const override { return "Reflects incident vector I across normal N. Result direction is I - 2*dot(N,I)*N."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialInput* I = nullptr;
        CMaterialInput* N = nullptr;
    };

    REFLECT()
    class CMaterialExpression_Refract : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Vector"; }
        FStringView GetNodeDisplayName() const override { return "Refract"; }
        FStringView GetNodeTooltip() const override { return "Snell-law refraction of I through normal N at relative IOR Eta."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialInput* I = nullptr;
        CMaterialInput* N = nullptr;
        CMaterialInput* Eta = nullptr;
    };

    REFLECT()
    class CMaterialExpression_RotateAboutAxis : public CMaterialExpression
    {
        GENERATED_BODY()
    public:
        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Vector"; }
        FStringView GetNodeDisplayName() const override { return "RotateAboutAxis"; }
        FStringView GetNodeTooltip() const override { return "Rotates Position around Axis by Angle (radians) using Rodrigues' formula."; }
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        CMaterialInput* Position = nullptr;
        CMaterialInput* Axis = nullptr;
        CMaterialInput* Angle = nullptr;
        CMaterialInput* Pivot = nullptr;
    };
}
