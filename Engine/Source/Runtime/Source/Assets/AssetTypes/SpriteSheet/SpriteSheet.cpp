#include "RuntimePCH.h"
#include "SpriteSheet.h"

namespace Lumina
{
    void SpriteAnimation::Advance(FSpritePlayback& Playback, int32 FrameCount, float FPS, bool bLoop,
                                  float SpeedScale, float Dt)
    {
        Playback.Frame = Math::Clamp(Playback.Frame, 0, FrameCount - 1);

        if (Playback.bFinished || FPS <= 0.0f || Dt <= 0.0f || SpeedScale <= 0.0f)
        {
            return;
        }

        Playback.Progress += Dt * FPS * SpeedScale;

        while (Playback.Progress >= 1.0f)
        {
            Playback.Progress -= 1.0f;

            if (Playback.Frame + 1 < FrameCount)
            {
                ++Playback.Frame;
                continue;
            }

            if (bLoop)
            {
                Playback.Frame = 0;
                continue;
            }

            // A clip that has ended holds its last frame rather than snapping back.
            Playback.bFinished = true;
            Playback.Progress  = 0.0f;
            break;
        }
    }

    const SSpriteAnimation* CSpriteSheet::FindAnimation(const FName& InName) const
    {
        if (InName.IsNone())
        {
            return GetDefaultAnimation();
        }

        for (const SSpriteAnimation& Animation : Animations)
        {
            if (Animation.Name == InName)
            {
                return &Animation;
            }
        }
        return nullptr;
    }

    const SSpriteAnimation* CSpriteSheet::GetDefaultAnimation() const
    {
        return Animations.empty() ? nullptr : &Animations[0];
    }
}
