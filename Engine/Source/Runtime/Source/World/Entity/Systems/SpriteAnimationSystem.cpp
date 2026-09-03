#include "RuntimePCH.h"
#include "SpriteAnimationSystem.h"

#include "Assets/AssetTypes/SpriteSheet/SpriteSheet.h"
#include "SystemContext.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/AnimatedSprite3DComponent.h"
#include "World/Entity/Components/Sprite3DComponent.h"

namespace Lumina
{
    FSystemAccess SSpriteAnimationSystem::Access = FSystemAccess{}
        .Write<SAnimatedSprite3DComponent, SSprite3DComponent>();

    void SSpriteAnimationSystem::Update(const FSystemContext& Context) noexcept
    {
        LUMINA_PROFILE_SCOPE();

        const float Dt = (float)Context.GetDeltaTime();

        Context.CreateView<SAnimatedSprite3DComponent, SSprite3DComponent>().ForEach(
            [Dt](SAnimatedSprite3DComponent& Animated, SSprite3DComponent& Sprite)
        {
            CSpriteSheet* Sheet = Animated.SpriteSheet.Get();
            if (Sheet == nullptr)
            {
                return;
            }

            const SSpriteAnimation* Clip = Sheet->FindAnimation(Animated.Animation);
            if (Clip == nullptr || Clip->Frames.empty())
            {
                return;
            }

            // Retargeting mid-clip would land on a frame index the new clip may not have.
            if (Animated.ActiveAnimation != Animated.Animation)
            {
                Animated.ActiveAnimation = Animated.Animation;
                Animated.Frame           = 0;
                Animated.FrameProgress   = 0.0f;
                Animated.bFinished       = false;
            }

            FSpritePlayback Playback;
            Playback.Frame     = Animated.Frame;
            Playback.Progress  = Animated.FrameProgress;
            Playback.bFinished = Animated.bFinished;

            if (Animated.bPlaying)
            {
                SpriteAnimation::Advance(Playback, (int32)Clip->Frames.size(), Clip->FPS, Clip->bLoop,
                                         Animated.SpeedScale, Dt);
            }
            else
            {
                Playback.Frame = Math::Clamp(Playback.Frame, 0, (int32)Clip->Frames.size() - 1);
            }

            Animated.Frame         = Playback.Frame;
            Animated.FrameProgress = Playback.Progress;
            Animated.bFinished     = Playback.bFinished;

            // The sheet owns the grid, so the sprite is driven rather than authored alongside it.
            Sprite.Texture        = Sheet->Texture;
            Sprite.HFrames        = Math::Max(Sheet->HFrames, 1);
            Sprite.VFrames        = Math::Max(Sheet->VFrames, 1);
            Sprite.bRegionEnabled = false;
            Sprite.Frame          = Math::Clamp(Clip->Frames[Animated.Frame], 0, Sheet->GetFrameCount() - 1);
        });
    }
}
