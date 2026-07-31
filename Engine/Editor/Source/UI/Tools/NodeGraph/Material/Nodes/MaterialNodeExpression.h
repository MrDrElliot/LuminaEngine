#pragma once
#include "MaterialGraphNode.h"
#include "UI/Tools/NodeGraph/Material/MaterialInput.h"
#include "UI/Tools/NodeGraph/Material/MaterialOutput.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"
#include "MaterialNodeExpression.generated.h"

namespace Lumina
{
    REFLECT()
    class CMaterialExpression : public CMaterialGraphNode
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;
        void DrawNodeTitleBar() override;

        // The FName this expression exposes as a material parameter, or null when the node has no
        // parameter to name. Mirrors the GetNodeDefaultValue() pointer idiom; non-const because the
        // title bar's inline rename writes through it.
        virtual FName* GetParameterName() { return nullptr; }

        CMaterialOutput* Output;

        /** When true, this expression's result varies per-instance at runtime via dynamic parameters. */
        PROPERTY(Editable, Category = "Dynamic")
        bool bDynamic = false;

    private:

        // Draws the F2 rename field in place of the title, writing Out on commit.
        void DrawParameterRename(FName& Out);

        // Transient (deliberately not a PROPERTY, so it never serializes): true only while this node's
        // title bar is an edit field.
        bool bRenamingParameter = false;
    };

    REFLECT()
    class CMaterialExpression_Math : public CMaterialExpression
    {
        GENERATED_BODY()

    public:

        void BuildNode() override;

        FFixedString GetNodeCategory() const override { return "Math"; }

        CMaterialInput* A = nullptr;
        CMaterialInput* B = nullptr;

        /** Constant value used for the A input when no pin is connected. */
        PROPERTY(Editable, Category = "Value")
        float ConstA = 0;

        /** Constant value used for the B input when no pin is connected. */
        PROPERTY(Editable, Category = "Value")
        float ConstB = 0;
    };
}
