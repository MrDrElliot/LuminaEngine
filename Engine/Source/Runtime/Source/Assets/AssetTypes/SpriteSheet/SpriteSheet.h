#pragma once

#include "Containers/Name.h"
#include "Containers/Vector.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectMacros.h"
#include "SpriteSheet.generated.h"

namespace Lumina
{
    class CTexture;

    /** One named clip. Frames are cell indices into the sheet grid, counted left to right then top to bottom. */
    REFLECT()
    struct RUNTIME_API SSpriteAnimation
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Animation")
        FName Name;

        PROPERTY(Editable, Category = "Animation", ClampMin = 0.0f)
        float FPS = 10.0f;

        PROPERTY(Editable, Category = "Animation")
        bool bLoop = true;

        PROPERTY(Editable, Category = "Animation")
        TVector<int32> Frames;
    };

    /** Where a clip has got to. Kept out of the component so the stepping can be tested without a world. */
    struct FSpritePlayback
    {
        int32 Frame     = 0;
        float Progress  = 0.0f;
        bool  bFinished = false;
    };

    namespace SpriteAnimation
    {
        /** Steps Playback over Dt. FrameCount must be positive. */
        RUNTIME_API void Advance(FSpritePlayback& Playback, int32 FrameCount, float FPS, bool bLoop,
                                 float SpeedScale, float Dt);
    }

    /** A texture sliced into a uniform grid, plus the named animations played over those cells. */
    REFLECT()
    class RUNTIME_API CSpriteSheet : public CObject
    {
        GENERATED_BODY()

    public:

        bool IsAsset() const override { return true; }

        NODISCARD const SSpriteAnimation* FindAnimation(const FName& InName) const;

        /** The first animation, so a component with no name set still plays something sensible. */
        NODISCARD const SSpriteAnimation* GetDefaultAnimation() const;

        NODISCARD int32 GetFrameCount() const { return Math::Max(HFrames, 1) * Math::Max(VFrames, 1); }

        PROPERTY(Editable, Category = "Sheet")
        TObjectPtr<CTexture> Texture;

        PROPERTY(Editable, Category = "Sheet", ClampMin = 1)
        int32 HFrames = 1;

        PROPERTY(Editable, Category = "Sheet", ClampMin = 1)
        int32 VFrames = 1;

        PROPERTY(Editable, Category = "Animations")
        TVector<SSpriteAnimation> Animations;
    };
}
