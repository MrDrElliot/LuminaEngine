#pragma once

#include "Containers/Name.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectMacros.h"
#include "AnimatedSprite3DComponent.generated.h"

namespace Lumina
{
    class CSpriteSheet;

    // Owns only playback; the SSprite3DComponent required beside it does the drawing.
    REFLECT(Component, Category = "Rendering")
    struct RUNTIME_API SAnimatedSprite3DComponent
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Animation")
        TObjectPtr<CSpriteSheet> SpriteSheet;

        /** Clip to play. Empty plays the sheet's first animation. */
        PROPERTY(Editable, Category = "Animation")
        FName Animation;

        PROPERTY(Editable, Category = "Animation")
        bool bPlaying = true;

        PROPERTY(Editable, Category = "Animation", ClampMin = 0.0f)
        float SpeedScale = 1.0f;

        /** Index into the clip's frame list, not the sheet grid. */
        PROPERTY(Editable, Category = "Animation", ClampMin = 0)
        int32 Frame = 0;

        /** Fraction of the current frame already elapsed. */
        float FrameProgress = 0.0f;

        /** Set when a clip that does not loop reaches its end. */
        bool bFinished = false;

        /** Last clip driven, so changing the name restarts instead of resuming mid-clip. */
        FName ActiveAnimation;
    };
}
