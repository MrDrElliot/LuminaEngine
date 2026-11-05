#include "RuntimePCH.h"
#include "SpriteAnimationSystem.h"

#include "Assets/AssetTypes/SpriteSheet/SpriteSheet.h"
#include "World/Entity/Components/AnimatedSprite3DComponent.h"
#include "World/Entity/Components/Sprite3DComponent.h"

namespace Lumina
{
    void SSpriteAnimationSystem::Configure()
    {
        RequireUpdate(EUpdateStage::PrePhysics);
    }

    // Every animated sprite also has a sprite, so a group lets the per-frame walk index both pools at one slot.
    void SSpriteAnimationSystem::OnStartup(const SExclusiveContext& Context)
    {
        (void)Context.GetRegistry().DeclareGroup<SAnimatedSprite3DComponent, SSprite3DComponent>();
    }

    void SSpriteAnimationSystem::OnUpdate(const SSystemContext& Context)
    {
        const float Dt = Context.GetDeltaTime();

        Animators.ParallelForEach([Dt](SAnimatedSprite3DComponent& Animated, SSprite3DComponent& Sprite)
        {
            CSpriteSheet* Sheet = Animated.SpriteSheet.Get();
            if (Sheet == nullptr)
            {
                return;
            }

            const SSpriteAnimation* Clip = Sheet->FindAnimation(Animated.Animation);
            if (Clip == nullptr || Clip->Frames.IsEmpty())
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
                SpriteAnimation::Advance(Playback, Clip->Frames.Num(), Clip->FPS, Clip->bLoop,
                                         Animated.SpeedScale, Dt);
            }
            else
            {
                Playback.Frame = Math::Clamp(Playback.Frame, 0, Clip->Frames.Num() - 1);
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
