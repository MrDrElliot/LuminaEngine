#include "RuntimePCH.h"
#include "SpriteAnimationSystem.h"

#include "Assets/AssetTypes/SpriteSheet/SpriteSheet.h"
#include "World/Entity/Components/AnimatedSprite3DComponent.h"
#include "World/Entity/Components/Sprite3DComponent.h"

namespace Lumina
{
    void CSpriteAnimationSystem::Configure()
    {
        RequireUpdate(EUpdateStage::PrePhysics);
    }

    // Every animated sprite also has a sprite, so a group lets the per-frame walk index both pools at one slot.
    void CSpriteAnimationSystem::OnStartup(const SExclusiveContext& Context)
    {
        (void)Context.GetRegistry().DeclareGroup<SAnimatedSprite3DComponent, SSprite3DComponent>();
    }

    void CSpriteAnimationSystem::OnUpdate(const SSystemContext& Context)
    {
        const float Dt = Context.GetDeltaTime();

        Animators.ParallelForEach([Dt](ECS::TMut<SAnimatedSprite3DComponent> AnimatedMut, ECS::TMut<SSprite3DComponent> SpriteMut)
        {
            const SAnimatedSprite3DComponent& Animated = AnimatedMut.Get();
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
                SAnimatedSprite3DComponent& Retargeted = AnimatedMut.GetMut();
                Retargeted.ActiveAnimation = Animated.Animation;
                Retargeted.Frame           = 0;
                Retargeted.FrameProgress   = 0.0f;
                Retargeted.bFinished       = false;
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

            AnimatedMut.SetIfChanged(&SAnimatedSprite3DComponent::Frame, Playback.Frame);
            AnimatedMut.SetIfChanged(&SAnimatedSprite3DComponent::FrameProgress, Playback.Progress);
            AnimatedMut.SetIfChanged(&SAnimatedSprite3DComponent::bFinished, Playback.bFinished);

            // The sheet owns the grid, so the sprite is driven rather than authored alongside it.
            SpriteMut.SetIfChanged(&SSprite3DComponent::Texture, Sheet->Texture);
            SpriteMut.SetIfChanged(&SSprite3DComponent::HFrames, Math::Max(Sheet->HFrames, 1));
            SpriteMut.SetIfChanged(&SSprite3DComponent::VFrames, Math::Max(Sheet->VFrames, 1));
            SpriteMut.SetIfChanged(&SSprite3DComponent::bRegionEnabled, false);
            SpriteMut.SetIfChanged(&SSprite3DComponent::Frame, Math::Clamp(Clip->Frames[Playback.Frame], 0, Sheet->GetFrameCount() - 1));
        });
    }
}
