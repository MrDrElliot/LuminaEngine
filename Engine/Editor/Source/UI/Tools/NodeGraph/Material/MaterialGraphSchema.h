#pragma once

#include "UI/Tools/NodeGraph/EdGraphSchema.h"

namespace Lumina
{
    /**
     * Connection rules for material graphs (and, by inheritance, material function graphs).
     *
     * Float widths stay deliberately permissive -- the compiler coerces and masks them, and half the
     * graph's ergonomics come from being able to drop a float into a float3 slot. The one rule here is
     * that a TextureHandle is not a float: it is a raw bindless index, and the shader arithmetic that
     * would silently accept it (uint promotes to float) produces garbage rather than an error. So a
     * handle only ever reaches a pin that asked for one.
     *
     * Reroutes are exempt on both sides. Their pins are plain float-typed passthroughs and the compiler
     * resolves back through them to the real producer (ResolveThroughReroutes), so type-checking them
     * would reject a wire that compiles correctly.
     */
    struct FMaterialGraphSchema : public FEdGraphSchema
    {
        bool CanCreateConnection(CEdNodeGraphPin* From, CEdNodeGraphPin* To) const override;
    };

    const FMaterialGraphSchema& GetMaterialGraphSchema();
}
