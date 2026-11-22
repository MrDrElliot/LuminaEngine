#pragma once
#include "MaterialNodeExpression.h"
#include "MaterialGraphNode.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Assets/AssetTypes/MaterialFunction/MaterialFunction.h"
#include "UI/Tools/NodeGraph/Material/MaterialGraphTypes.h"
#include "MaterialNode_Function.generated.h"

namespace Lumina
{
    class CMaterialInput;

    // The type helpers live in MaterialGraphTypes.h so any node can use them without this header.

    // Declares one input of the owning material function, bound to the caller's argument on inline.
    REFLECT()
    class CMaterialExpression_FunctionInput : public CMaterialExpression
    {
        GENERATED_BODY()
    public:

        // Only meaningful inside a function graph, so it stays out of a plain material's palette.
        CClass* GetSupportedGraphClass() const override;

        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Function"; }
        FStringView GetNodeDisplayName() const override { return "FunctionInput"; }
        FStringView GetNodeTooltip() const override { return "An input parameter of this material function."; }
        uint32 GetNodeTitleColor() const override { return IM_COL32(35, 140, 90, 255); }
        void DrawNodeBody() override;
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        /** Identifier for this input. Must be unique within the function; drives the call node's pin. */
        PROPERTY(Editable, Category = "Function Input")
        FName InputName = "In";

        PROPERTY(Editable, Category = "Function Input")
        EMaterialValueType InputType = EMaterialValueType::Float;

        /** Value used when a call node leaves this input unconnected (and for in-editor preview). */
        PROPERTY(Editable, Category = "Function Input")
        FVector4 DefaultValue = FVector4(0.0f);

        /** Lower values sort earlier in the call node's pin list. */
        PROPERTY(Editable, Category = "Function Input")
        int32 SortPriority = 0;

        PROPERTY(Editable, Category = "Function Input")
        FString Description;

        // Refreshes the output pin's type from InputType so downstream nodes see the right width.
        void DrawNodeTitleBar() override;
    };

    // Declares one output of the owning material function, read by the call node on inline.
    REFLECT()
    class CMaterialFunctionOutput : public CMaterialGraphNode
    {
        GENERATED_BODY()
    public:

        // Function graphs only, for the same reason as CMaterialExpression_FunctionInput.
        CClass* GetSupportedGraphClass() const override;

        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Function"; }
        FStringView GetNodeDisplayName() const override { return "FunctionOutput"; }
        FStringView GetNodeTooltip() const override { return "An output of this material function."; }
        uint32 GetNodeTitleColor() const override { return IM_COL32(140, 70, 35, 255); }
        bool IsDeletable() const override { return true; }
        void DrawNodeBody() override;
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        /** Identifier for this output. Must be unique within the function; drives the call node's pin. */
        PROPERTY(Editable, Category = "Function Output")
        FName OutputName = "Out";

        PROPERTY(Editable, Category = "Function Output")
        EMaterialValueType OutputType = EMaterialValueType::Float;

        /** Lower values sort earlier in the call node's pin list. */
        PROPERTY(Editable, Category = "Function Output")
        int32 SortPriority = 0;

        PROPERTY(Editable, Category = "Function Output")
        FString Description;

        CMaterialInput* Input = nullptr;
    };

    // Calls a material function by inlining its body at compile time.
    REFLECT()
    class CMaterialExpression_MaterialFunctionCall : public CMaterialGraphNode
    {
        GENERATED_BODY()
    public:

        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Function"; }
        FStringView GetNodeDisplayName() const override { return "MaterialFunction"; }
        FStringView GetNodeTooltip() const override { return "Inlines a Material Function asset, exposing its inputs and outputs as pins."; }
        uint32 GetNodeTitleColor() const override { return IM_COL32(80, 110, 170, 255); }
        ImVec2 GetMinNodeBodySize() const override { return ImVec2(120, 60); }

        void DrawNodeBody() override;
        void DrawNodeTitleBar() override;
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        /** The material function asset to inline. Changing it rebuilds this node's pins. */
        PROPERTY(Editable, Category = "Material Function")
        TStrongObjectPtr<CMaterialFunction> Function;

        // Keeps connections that still match by pin name, and name-hashed pin IDs survive save and load.
        void RebuildPins();
        bool NeedsPinRebuild() const;

        // The input/output pins, in signature order. Parallel to Function's Inputs/Outputs.
        TVector<CMaterialInput*>  FunctionInputPins;
        TVector<CMaterialOutput*> FunctionOutputPins;

    private:

        // Detects a Function change (or first build) so DrawNodeTitleBar can rebuild pins lazily.
        CMaterialFunction* CachedFunction = nullptr;
        TVector<EMaterialValueType> CachedInputTypes;
        TVector<EMaterialValueType> CachedOutputTypes;
        bool               bPinsBuilt = false;
    };
}
