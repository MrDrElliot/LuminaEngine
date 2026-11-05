#include "RuntimePCH.h"
#include "AudioSystem.h"
#include "CameraSystem.h"
#include "Assets/AssetTypes/Audio/AudioStream.h"
#include "Audio/AudioGlobals.h"
#include "Audio/AudioSettings.h"
#include "Audio/SoundPlayback.h"
#include "Core/Object/ObjectCore.h"
#include "Physics/PhysicsScene.h"

namespace Lumina
{
    void SAudioSystem::Configure()
    {
        RequireUpdate(EUpdateStage::FrameEnd);
        RunsAfter<SCameraSystem>();
    }

	namespace AudioSystemInternal
	{
		float MoveTowards(float Current, float Target, float MaxDelta)
		{
			if (Math::Abs(Target - Current) <= MaxDelta)
			{
				return Target;
			}
			return Current + (Target > Current ? MaxDelta : -MaxDelta);
		}

		struct FVolumeGroupWinner
		{
			FName        Group;
			ECS::FEntity Entity = ECS::NullEntity;
			int32        Priority = 0;
			float        Weight = 0.0f;
		};

		// Each volume fades toward its own weight when it is ungrouped or wins its group, and toward silence otherwise.
		template <typename TVolumeQuery, typename TTransformQuery>
		void UpdateAudioVolumes(TVolumeQuery& Volumes, const TTransformQuery& Transforms, const FVector3& Listener, float DeltaTime)
		{
			TVector<FVolumeGroupWinner> Winners;
			TVector<SAudioVolumeComponent*> GroupVoices;
			Volumes.ForEach([&](ECS::FEntity Entity, SAudioVolumeComponent& Volume)
			{
				if (Volume.bPlaying && !Volume.Group.IsNone())
				{
					GroupVoices.Add(&Volume);
				}

				const STransformComponent* Xform = Volume.bEnabled ? Transforms.template TryGet<STransformComponent>(Entity) : nullptr;
				Volume.InsideWeight = Xform != nullptr ? Volume.ListenerWeight(*Xform, Listener) : 0.0f;
				Volume.bListenerInside = Volume.InsideWeight > 0.0f;
				if (!Volume.bListenerInside)
				{
					Volume.bFinishedThisVisit = false;
					return;
				}
				if (Volume.Group.IsNone())
				{
					return;
				}
				FVolumeGroupWinner* Winner = nullptr;
				for (FVolumeGroupWinner& Candidate : Winners)
				{
					if (Candidate.Group == Volume.Group)
					{
						Winner = &Candidate;
						break;
					}
				}
				if (Winner == nullptr)
				{
					Winners.Emplace(Volume.Group, Entity, Volume.Priority, Volume.InsideWeight);
				}
				else if (Volume.Priority > Winner->Priority || (Volume.Priority == Winner->Priority && Volume.InsideWeight > Winner->Weight))
				{
					*Winner = FVolumeGroupWinner{ Volume.Group, Entity, Volume.Priority, Volume.InsideWeight };
				}
			});

			Volumes.ForEach([&](ECS::FEntity Entity, SAudioVolumeComponent& Volume)
			{
				bool bWinsGroup = Volume.Group.IsNone();
				for (const FVolumeGroupWinner& Winner : Winners)
				{
					if (Winner.Group == Volume.Group)
					{
						bWinsGroup = Winner.Entity == Entity;
						break;
					}
				}

				const float Target = (Volume.bListenerInside && bWinsGroup) ? Volume.InsideWeight : 0.0f;
				const float FadeTime = Target > Volume.Gain ? Volume.FadeInTime : Volume.FadeOutTime;
				Volume.Gain = MoveTowards(Volume.Gain, Target, FadeTime > 0.0f ? DeltaTime / FadeTime : 1.0f);

				// A one-shot that played out stays quiet until the listener leaves and comes back.
				if (Volume.bPlaying && Audio::Context().GetVoiceState(Volume.ActiveHandle) == EAudioVoiceState::Free)
				{
					Volume.bPlaying = false;
					Volume.ActiveHandle = SAudioHandle::Invalid();
					Volume.ResumeFrame = 0;
					Volume.bFinishedThisVisit = !Volume.bLooping;
				}

				// Neighboring volumes of a group that play the same sound pass the voice along instead of restarting it.
				if (!Volume.bPlaying && Target > 0.0f && !Volume.Group.IsNone())
				{
					for (SAudioVolumeComponent* Other : GroupVoices)
					{
						if (Other != &Volume && Other->bPlaying && Other->Group == Volume.Group && Other->Sound == Volume.Sound)
						{
							Volume.ActiveHandle  = Other->ActiveHandle;
							Volume.Gain          = Math::Max(Volume.Gain, Other->Gain);
							Volume.AppliedVolume = Other->AppliedVolume;
							Volume.bPlaying      = true;
							Other->ActiveHandle  = SAudioHandle::Invalid();
							Other->bPlaying      = false;
							Other->Gain          = 0.0f;
							break;
						}
					}
				}

				const float Wanted = Volume.Volume * Volume.Gain;
				if (!Volume.bPlaying)
				{
					if (Volume.Gain > 0.0f && !Volume.bFinishedThisVisit && Volume.Sound != nullptr && Volume.Sound->IsPlayable())
					{
						SAudioPlayParams Params;
						Params.Volume       = Wanted;
						Params.bLooping     = Volume.bLooping;
						Params.bSpatialized = false;
						Params.Bus          = Volume.Bus;
						Params.StartFrame   = Volume.bResumePlayback ? Volume.ResumeFrame : 0;
						Volume.ActiveHandle  = Audio::PlaySound(Volume.Sound.Get(), Params).Handle;
						Volume.bPlaying      = Volume.ActiveHandle.IsValid();
						Volume.AppliedVolume = Wanted;
					}
					return;
				}

				if (Volume.Gain <= 0.0f)
				{
					Volume.StopVoice();
				}
				else if (Math::Abs(Wanted - Volume.AppliedVolume) > 0.002f)
				{
					Audio::Context().SetVolume(Volume.ActiveHandle, Wanted);
					Volume.AppliedVolume = Wanted;
				}
			});
		}
	}

	bool SAudioSystem::HasWork(const SSystemContext& Context)
	{
		return !Listeners.IsEmpty() || !Volumes.IsEmpty() || !Sources.IsEmpty() || !ProceduralSources.IsEmpty();
	}

	void SAudioSystem::OnTeardown(const SExclusiveContext& Context)
	{

		// No audio device in a headless dedicated server (Audio::Initialize is skipped).
		if (!Audio::HasDevice())
		{
			return;
		}

		// Stop all sounds owned by audio source components in this world.
		AllSources.ForEach([](SAudioSourceComponent& Audio)
		{
			if (Audio.bPlaying && Audio.ActiveHandle.IsValid())
			{
				Audio::Context().StopSound(Audio.ActiveHandle);
				Audio.ActiveHandle = SAudioHandle::Invalid();
				Audio.bPlaying = false;
			}
		});

		ProceduralSources.ForEach([](SProceduralAudioComponent& Audio)
		{
			if (Audio.bPlaying && Audio.ActiveHandle.IsValid())
			{
				Audio::Context().StopSound(Audio.ActiveHandle);
				Audio.ActiveHandle = SAudioHandle::Invalid();
				Audio.bPlaying = false;
			}
		});

		Volumes.ForEach([](SAudioVolumeComponent& Volume)
		{
			Volume.ResumeFrame = 0;
			Volume.StopVoice();
			Volume.Gain = 0.0f;
		});
	}

	void SAudioSystem::OnUpdate(const SSystemContext& Context)
	{
		using namespace AudioSystemInternal;

		// No audio device in a headless dedicated server (Audio::Initialize is skipped).
		if (!Audio::HasDevice())
		{
			return;
		}

		const CAudioSettings* Settings = GetDefault<CAudioSettings>();
		const float DeltaTime = Context.GetDeltaTime();
		const FKinematicsState& Kinematics = *KinematicsState;

		FVector3 ListenerPosition(0.0f);
		bool bHasListener = false;
		uint32 DrivenListenerMask = 0;

		const ECS::FEntity ActiveCamera = CameraState->ActiveCameraEntity;
		const bool bCameraHasTransform = ActiveCamera != ECS::NullEntity && !ActiveCamera.IsTombstone() && Transforms.Contains(ActiveCamera);

		{
			Listeners.ForEach([&](ECS::FEntity Entity, const SAudioListenerComponent& Listener)
			{
				const uint32 Index = (uint32)Math::Clamp(Listener.ListenerIndex, 0, 3);
				const ECS::FEntity Ears = (Listener.bFollowActiveCamera && Index == 0 && bCameraHasTransform) ? ActiveCamera : Entity;
				const STransformComponent& Transform = Transforms.Get<STransformComponent>(Ears);
				const FVector3 Position = Transform.GetWorldLocation();

				const FVector3 Velocity = Kinematics.GetVelocity(Ears);

				Audio::Context().UpdateListener(Index, Position, Transform.GetWorldRotation(),
					Listener.bApplyDoppler ? Velocity : FVector3(0.0f));

				if (!bHasListener || Index == 0)
				{
					ListenerPosition = Position;
					bHasListener = true;
				}

				DrivenListenerMask |= (1u << Index);
			});
		}

		// Only take over the listener slots once a world drives one, so preview scenes keep default 0.
		if (bHasListener)
		{
			for (uint32 Index = 0; Index < Audio::Context().GetListenerCount(); ++Index)
			{
				Audio::Context().SetListenerEnabled(Index, (DrivenListenerMask & (1u << Index)) != 0);
			}
		}

		if (bHasListener)
		{
			UpdateAudioVolumes(Volumes, Transforms, ListenerPosition, DeltaTime);
		}

		const bool bOcclusionAllowed = bHasListener && Settings != nullptr && Settings->bOcclusionEnabled;
		Physics::IPhysicsQueries* PhysicsScene = bOcclusionAllowed ? Context.GetPhysics() : nullptr;
		uint32 TraceBudget = (Settings != nullptr) ? Settings->MaxOcclusionTracesPerTick : 0;
		const FSignificanceState& Significance = *SignificanceState;

		{
			Sources.ForEach([&](ECS::FEntity Entity, SAudioSourceComponent& Audio, const STransformComponent& Transform)
			{
				const FVector3 Position = Transform.GetWorldLocation();
				Audio.LastPosition = Position;

				// The mixer may have retired the voice (one-shot ended, evicted by a higher priority).
				if (Audio.bPlaying && Audio::Context().GetVoiceState(Audio.ActiveHandle) == EAudioVoiceState::Free)
				{
					Audio.bPlaying = false;
					Audio.bPaused = false;
					Audio.ActiveHandle = SAudioHandle::Invalid();
				}

				const bool bInRange = !Audio.bSpatialized || !Audio.bCullBeyondMaxDistance || !bHasListener ||
					Math::Distance(Position, ListenerPosition) <= Audio.Attenuation.Resolve().MaxDistance;

				if (!Audio.bReady)
				{
					Audio.bReady = true;

					if (Audio.bPlayOnReady && Audio.Sound != nullptr && Audio.Sound->IsPlayable() && bInRange)
					{
						Audio.Play();
					}
					return;
				}

				// A looping sound virtualizes out of range; a one shot must not, or it would restart every time it finished.
				if (Audio.bPlayOnReady && Audio.Sound != nullptr && Audio.Sound->IsPlayable() && Audio.IsPersistent())
				{
					if (!Audio.bPlaying && bInRange)
					{
						Audio.Play();
					}
					else if (Audio.bPlaying && Audio.bSpatialized && Audio.bCullBeyondMaxDistance && bHasListener &&
						Math::Distance(Position, ListenerPosition) > Audio.Attenuation.Resolve().MaxDistance * 1.1f)
					{
						Audio.StopWithMode(EAudioStopMode::Immediate);
					}
				}

				if (!Audio.bPlaying || !Audio.ActiveHandle.IsValid())
				{
					return;
				}

				const FVector3 Velocity = Kinematics.GetVelocity(Entity);

				if (Audio.bSpatialized)
				{
					Audio::Context().SetPosition(Audio.ActiveHandle, Position);

					if (Audio.Attenuation.Resolve().DopplerFactor > 0.0f)
					{
						Audio::Context().SetVelocity(Audio.ActiveHandle, Velocity);
					}
				}

				if (Audio.bVolumeDirty)
				{
					Audio::Context().SetVolume(Audio.ActiveHandle, Audio.Volume);
					Audio.bVolumeDirty = false;
				}

				if (Audio.bPitchDirty)
				{
					Audio::Context().SetPitch(Audio.ActiveHandle, Audio.Pitch);
					Audio.bPitchDirty = false;
				}

				if (Audio.bLoopingDirty)
				{
					Audio::Context().SetLooping(Audio.ActiveHandle, Audio.bLooping);
					Audio.bLoopingDirty = false;
				}

				if (Audio.bAttenuationDirty)
				{
					Audio::Context().SetAttenuation(Audio.ActiveHandle, Audio.Attenuation.Resolve());
					Audio.bAttenuationDirty = false;
				}

				if (!Audio.Occlusion.bEnabled || !Audio.bSpatialized || !bOcclusionAllowed)
				{
					return;
				}

				Audio.OcclusionTraceTimer -= DeltaTime;
				if (Audio.OcclusionTraceTimer <= 0.0f && PhysicsScene != nullptr && TraceBudget > 0)
				{
					--TraceBudget;
					Audio.OcclusionTraceTimer = Audio.Occlusion.TraceInterval * (float)Significance.Get(Entity).TickInterval;

					SRayCastSettings Trace;
					Trace.Start     = ListenerPosition;
					Trace.End       = Position;
					Trace.LayerMask = Settings->OcclusionTraceChannel;

					const ECS::FEntity BodyID = Entity;
					if (BodyID != ECS::NullEntity)
					{
						Trace.IgnoreEntities.Add(BodyID);
					}

					Audio.OcclusionTarget = PhysicsScene->CastRay(Trace).IsSet() ? 1.0f : 0.0f;
				}

				const float PreviousOcclusion = Audio.OcclusionCurrent;
				const float Rate = Audio.Occlusion.InterpTime > 0.0f ? (DeltaTime / Audio.Occlusion.InterpTime) : 1.0f;
				Audio.OcclusionCurrent = MoveTowards(Audio.OcclusionCurrent, Audio.OcclusionTarget, Rate);

				if (Math::Abs(Audio.OcclusionCurrent - PreviousOcclusion) > 0.001f)
				{
					Audio::Context().SetOcclusion(Audio.ActiveHandle, Audio.OcclusionCurrent,
						Audio.Occlusion.LowPassFrequency, Audio.Occlusion.VolumeAttenuation);
				}
			});
		}

		{
			ProceduralSources.ForEach([&](ECS::FEntity Entity, SProceduralAudioComponent& Audio)
			{
				if (!Audio.bReady)
				{
					Audio.bReady = true;

					if (Audio.bPlayOnReady)
					{
						Audio.Start();
					}
				}

				if (Audio.bPlaying && Audio::Context().GetVoiceState(Audio.ActiveHandle) == EAudioVoiceState::Free)
				{
					Audio.bPlaying = false;
					Audio.ActiveHandle = SAudioHandle::Invalid();
				}

				if (Audio.bPlaying && Audio.ActiveHandle.IsValid())
				{
					if (Audio.bSpatialized)
					{
						const STransformComponent& Transform = Transforms.Get<STransformComponent>(Entity);
						Audio::Context().SetPosition(Audio.ActiveHandle, Transform.GetWorldLocation());
					}

					if (Audio.bVolumeDirty)
					{
						Audio::Context().SetVolume(Audio.ActiveHandle, Audio.Volume);
						Audio.bVolumeDirty = false;
					}

					if (Audio.bPitchDirty)
					{
						Audio::Context().SetPitch(Audio.ActiveHandle, Audio.Pitch);
						Audio.bPitchDirty = false;
					}
				}
			});
		}
	}
}
