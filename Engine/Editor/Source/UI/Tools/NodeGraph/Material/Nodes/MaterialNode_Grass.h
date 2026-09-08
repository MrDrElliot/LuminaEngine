#pragma once

#include "MaterialGraphNode.h"
#include "Renderer/MaterialTypes.h"
#include "MaterialNode_Grass.generated.h"

namespace Lumina
{
    /** One species this node scatters, and the painted layer that decides where. */
    REFLECT()
    struct FGrassOutputEntry
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Grass")
        TObjectPtr<CGrassType> GrassType;

        /** Index into the terrain's Layers array; only that layer's painted weight grows this species. */
        PROPERTY(Editable, Category = "Grass", ClampMin = 0, ClampMax = 3)
        uint32 LayerIndex = 0;

        PROPERTY(Editable, Category = "Grass", ClampMin = 0.0f, NoDrag, Delta = 0.1f)
        float DensityScale = 1.0f;
    };

    /**
     * Publishes grass species for a terrain material to scatter. Contributes no shader code: the graph
     * compile copies these entries onto the material asset, and the runtime scatter reads them from
     * there, which is what lets grass exist without the editor loaded.
     */
    REFLECT()
    class CMaterialExpression_GrassOutput : public CMaterialGraphNode
    {
        GENERATED_BODY()

    public:

        uint32 GetNodeTitleColor() const override { return IM_COL32(55, 125, 55, 255); }
        FFixedString GetNodeCategory() const override { return "Terrain"; }
        FStringView GetNodeDisplayName() const override { return "GrassOutput"; }
        FStringView GetNodeTooltip() const override
        {
            return "Scatters grass meshes on the terrain layers named here. Emits no shader code; the "
                   "entries are baked onto the material for the runtime scatter to read.";
        }

        void BuildNode() override;

        /** Never reached: nothing downstream samples this node, so the compiler never emits it. */
        void GenerateDefinition(FMaterialCompiler& Compiler) override {}

        PROPERTY(Editable, Category = "Grass")
        TVector<FGrassOutputEntry> Entries;
    };
}
