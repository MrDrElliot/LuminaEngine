#pragma once

#include "Core/Object/ObjectMacros.h"
#include "WindBlockerComponent.generated.h"

namespace Lumina
{
    // A box the weather's wind does not reach, such as the inside of a building or a sheltered valley floor.
    REFLECT(Component, Category = "Environment")
    struct RUNTIME_API SWindBlockerComponent
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Wind")
        bool bEnabled = true;

        PROPERTY(Editable, Category = "Wind", Units = "m", ToolTip = "Half size of the sheltered box in local space, scaled by the entity's transform.")
        FVector3 Extent = FVector3(2.0f);

        PROPERTY(Editable, Category = "Wind", Units = "m", ClampMin = 0.0f, ClampMax = 50.0f, ToolTip = "Distance inside the box over which the wind dies away, so the shelter has a soft edge.")
        float Falloff = 1.0f;

        PROPERTY(Editable, Category = "Wind", ClampMin = 0.0f, ClampMax = 1.0f, ToolTip = "Share of the wind the box stops, 1 for still air inside.")
        float Strength = 1.0f;
    };
}
