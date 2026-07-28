#pragma once

#include "MaterialGraphNode.h"
#include "Assets/AssetTypes/MaterialFunction/MaterialFunction.h"
#include "UI/Tools/NodeGraph/Material/MaterialGraphTypes.h"
#include "MaterialNode_CustomSlang.generated.h"

namespace Lumina
{
    class CMaterialInput;
    class CMaterialOutput;

    // What an unconnected input falls back to. These are the material graph's stage-agnostic aliases
    // (declared for the pixel stage natively and mapped in the vertex preamble), so any of them is
    // valid wherever the node compiles. Lets a node do something useful with nothing wired in.
    REFLECT()
    enum class ECustomSlangInputDefault : uint8
    {
        Zero,
        UV0,
        WorldPosition,
        WorldNormal,
        VertexColor,
    };

    // One user-declared input or output of a Custom Slang node. Name becomes a real identifier in the
    // emitted shader block, so it is validated (identifier syntax, keyword, uniqueness) at compile time.
    REFLECT()
    struct FCustomSlangPin
    {
        GENERATED_BODY()

        /** Identifier your code sees. Must be a valid Slang identifier and unique across this node's pins. */
        PROPERTY(Editable, Category = "Custom Slang")
        FName Name = "In";

        /** Width of the value. Inputs are coerced to this from whatever is wired in. */
        PROPERTY(Editable, Category = "Custom Slang")
        EMaterialValueType Type = EMaterialValueType::Float;

        /** Value an unconnected input resolves to. Ignored on outputs. */
        PROPERTY(Editable, Category = "Custom Slang")
        ECustomSlangInputDefault Default = ECustomSlangInputDefault::Zero;
    };

    /**
     * Runs hand-written Slang inside the material graph.
     *
     * The material compiler substitutes its output INSIDE a shader function body, and Slang has no
     * nested function definitions, so the body cannot be emitted as a callable function. It is emitted
     * as a scoped block instead:
     *
     *     float3 CS12_out0 = float3(0,0,0);        // canonical, mangled -- what downstream reads
     *     {
     *         const float2 UV = (float2)(<upstream>);   // declared inputs, const
     *         float3 Color = float3(0,0,0);             // declared outputs
     *         { <your code> }
     *         CS12_out0 = Color;
     *     }
     *
     * Only the mangled names escape, so user identifiers shadow rather than redefine, and inputs are
     * const so upstream expressions can't be written back through. Validation runs before emission and
     * reports through the node; a rejected node still emits zeroed outputs so the shader keeps building.
     */
    REFLECT()
    class CMaterialExpression_CustomSlang : public CMaterialGraphNode
    {
        GENERATED_BODY()
    public:

        void BuildNode() override;
        FFixedString GetNodeCategory() const override { return "Custom"; }
        FStringView GetNodeDisplayName() const override { return "Custom Slang"; }
        FStringView GetNodeTooltip() const override { return "Write raw Slang with your own declared inputs and outputs."; }
        uint32 GetNodeTitleColor() const override { return IM_COL32(140, 65, 140, 255); }
        ImVec2 GetMinNodeBodySize() const override { return ImVec2(190, 74); }

        void DrawNodeBody() override;
        void DrawNodeTitleBar() override;
        void GenerateDefinition(FMaterialCompiler& Compiler) override;

        /** Shown on the node so several custom nodes stay tellable apart. Cosmetic only. */
        PROPERTY(Editable, Category = "Custom Slang")
        FName Title = "Custom";

        /** Declared inputs; each becomes an input pin and a const local your code can read. */
        PROPERTY(Editable, Category = "Custom Slang")
        TVector<FCustomSlangPin> Inputs;

        /** Declared outputs; each becomes an output pin and a local your code assigns to. */
        PROPERTY(Editable, Category = "Custom Slang")
        TVector<FCustomSlangPin> Outputs;

        /** Slang statements. Assign to your declared output names; no function definitions or preprocessor. */
        PROPERTY(Editable, Category = "Custom Slang", Multiline)
        FString Code = "Out = float3(UV, 0.0);\n";

        // Set once the starter signature has been installed, and serialized, so a node whose pins were
        // deliberately deleted stays deleted instead of re-seeding itself on the next load.
        PROPERTY()
        bool bDefaultsSeeded = false;

        // (Re)creates pins from Inputs/Outputs, preserving wires whose pin name still exists.
        void RebuildPins();

        // Checks the declared signature and the body. Returns false with one message per problem;
        // callers emit these as node errors. Pure -- no emission, safe to call from UI.
        bool Validate(TVector<FString>& OutProblems) const;

        TVector<CMaterialInput*>  CustomInputPins;
        TVector<CMaterialOutput*> CustomOutputPins;

    private:

        // Hash of the declared signature; a change means the pins are stale and get rebuilt on draw.
        uint64 SignatureHash = 0;
        bool   bPinsBuilt = false;

        uint64 ComputeSignatureHash() const;
    };
}
