#pragma once

#include "Core/Object/ObjectMacros.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "BillboardComponent.generated.h"

namespace Lumina
{
    class CTexture;
    
    REFLECT(Component, Category = "Rendering")
    struct SBillboardComponent
    {
        GENERATED_BODY()
        
        /** Texture displayed on the billboard quad. */
        PROPERTY(Editable)
        TObjectPtr<CTexture> Texture;

        /** Uniform scale of the billboard quad in world space. */
        PROPERTY(Editable)
        float Scale = 1.0f;

        /** RGBA tint multiplied with the billboard texture color. */
        PROPERTY(Editable, Color)
        FVector4 Tint;
    };
}
