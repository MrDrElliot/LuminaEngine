#pragma once
#include "MaterialNodeExpression.h"
#include "MaterialNode_StaticSwitch.generated.h"

namespace Lumina
{
    // Reports IsRerouteNode so every reroute walk resolves through the taken branch, dropping the other.
    REFLECT(MinimalAPI)
    class CMaterialExpression_StaticSwitch : public CMaterialExpression
    {
        GENERATED_BODY()
    public:

        void BuildNode() override;

        FFixedString GetNodeCategory() const override { return "Conditional"; }
        FStringView GetNodeDisplayName() const override { return "Static Switch"; }
        FStringView GetNodeTooltip() const override
        {
            return "Picks True or False at compile time. The branch not taken is never compiled, so it "
                   "costs no instructions, no texture slots and no parameter slots. Name it to let a "
                   "material instance switch it, which compiles that instance its own shader.";
        }

        uint32 GetNodeTitleColor() const override { return IM_COL32(120, 70, 160, 255); }

        void DrawNodeBody() override;
        void DrawContextMenu() override;

        FName* GetParameterName() override { return &ParameterName; }

        bool IsRerouteNode() const override { return true; }
        bool WantsRerouteDotRendering() const override { return false; }

        // The branch pins carry real types, so the schema still has to police what connects to them.
        bool IsUntypedPassthrough() const override { return false; }

        CEdNodeGraphPin* GetRerouteSourcePin() const override;

        /** Value the compile is running with, stamped by FMaterialCompiler::ResolveStaticSwitches. */
        void SetResolvedValue(bool bValue) { bResolvedValue = bValue; }
        bool GetResolvedValue() const { return bResolvedValue; }

        /** Name an instance overrides this switch by; unnamed switches are fixed at the master. */
        PROPERTY(Editable, Category = "Parameter")
        FName ParameterName;

        /** Branch taken when nothing overrides this switch. */
        PROPERTY(Editable, Category = "Value")
        bool bDefaultValue = true;

        CMaterialInput* True = nullptr;
        CMaterialInput* False = nullptr;

    private:

        // Deliberately not serialized, since it belongs to the permutation being compiled, not the asset.
        bool bResolvedValue = true;
    };
}
