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
    void CAudioSystem::Configure()
    {
        RequireUpdate(EUpdateStage::FrameEnd);
        RunsAfter<CCameraSystem>();
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

		struct FGroupVoice
		{
			ECS::FEntity                 Entity;
			const SAudioVolumeComponent* Volume = nullptr;
		};

		// Each volume fades toward its own weight when it is ungrouped or wins its group, and toward silence otherwise.
		template <typename TVolumeQuery, typename TTransformQuery>
		void UpdateAudioVolumes(TVolumeQuery& Volumes, const TTransformQuery& Transforms, const FVector3& Listener, float DeltaTime)
		{
			TVector<FVolumeGroupWinner> Winners;
			TVector<FGroupVoice> GroupVoices;
			Volumes.ForEach([&](ECS::FEntity Entity, ECS::TMut<SAudioVolumeComponent> Volume)
			{
				const SAudioVolumeComponent& Current = Volume.Get();
				if (Current.bPlaying && !Current.Group.IsNone())
				{
					GroupVoices.Add(FGroupVoice{ Entity, &Current });
				}

				const STransformComponent* Xform = Current.bEnabled ? Transforms.template TryGet<STransformComponent>(Entity) : nullptr;
				Volume.SetIfChanged(&SAudioVolumeComponent::InsideWeight, Xform != nullptr ? Current.ListenerWeight(*Xform, Listener) : 0.0f);
				Volume.SetIfChanged(&SAudioVolumeComponent::bListenerInside, Current.InsideWeight > 0.0f);
				if (!Current.bListenerInside)
				{
					Volume.SetIfChanged(&SAudioVolumeComponent::bFinishedThisVisit, false);
					return;
				}
				if (Current.Group.IsNone())
				{
					return;
				}
				FVolumeGroupWinner* Winner = nullptr;
				for (FVolumeGroupWinner& Candidate : Winners)
				{
					if (Candidate.Group == Current.Group)
					{
						Winner = &Candidate;
						break;
					}
				}
				if (Winner == nullptr)
				{
					Winners.Emplace(Current.Group, Entity, Current.Priority, Current.InsideWeight);
				}
				else if (Current.Priority > Winner->Priority || (Current.Priority == Winner->Priority && Current.InsideWeight > Winner->Weight))
				{
					*Winner = FVolumeGroupWinner{ Current.Group, Entity, Current.Priority, Current.InsideWeight };
				}
			});

			Volumes.ForEach([&](ECS::FEntity Entity, ECS::TMut<SAudioVolumeComponent> Volume)
			{
				const SAudioVolumeComponent& Current = Volume.Get();
				bool bWinsGroup = Current.Group.IsNone();
				for (const FVolumeGroupWinner& Winner : Winners)
				{
					if (Winner.Group == Current.Group)
					{
						bWinsGroup = Winner.Entity == Entity;
						break;
					}
				}

				const float Target = (Current.bListenerInside && bWinsGroup) ? Current.InsideWeight : 0.0f;
				const float FadeTime = Target > Current.Gain ? Current.FadeInTime : Current.FadeOutTime;
				Volume.SetIfChanged(&SAudioVolumeComponent::Gain, MoveTowards(Current.Gain, Target, FadeTime > 0.0f ? DeltaTime / FadeTime : 1.0f));

				// A one-shot that played out stays quiet until the listener leaves and comes back.
				if (Current.bPlaying && Audio::Context().GetVoiceState(Current.ActiveHandle) == EAudioVoiceState::Free)
				{
					Volume->bPlaying = false;
					Volume->ActiveHandle = SAudioHandle::Invalid();
					Volume->ResumeFrame = 0;
					Volume->bFinishedThisVisit = !Current.bLooping;
				}

				// Neighboring volumes of a group that play the same sound pass the voice along instead of restarting it.
				if (!Current.bPlaying && Target > 0.0f && !Current.Group.IsNone())
				{
					for (const FGroupVoice& Voice : GroupVoices)
					{
						const SAudioVolumeComponent& Other = *Voice.Volume;
						if (&Other != &Current && Other.bPlaying && Other.Group == Current.Group && Other.Sound == Current.Sound)
						{
							Volume->ActiveHandle  = Other.ActiveHandle;
							Volume->Gain          = Math::Max(Current.Gain, Other.Gain);
							Volume->AppliedVolume = Other.AppliedVolume;
							Volume->bPlaying      = true;
							SAudioVolumeComponent& Handoff = Volumes.template Get<SAudioVolumeComponent>(Voice.Entity);
							Handoff.ActiveHandle = SAudioHandle::Invalid();
							Handoff.bPlaying     = false;
							Handoff.Gain         = 0.0f;
							break;
						}
					}
				}

				const float Wanted = Current.Volume * Current.Gain;
				if (!Current.bPlaying)
				{
					if (Current.Gain > 0.0f && !Current.bFinishedThisVisit && Current.Sound != nullptr && Current.Sound->IsPlayable())
					{
						SAudioPlayParams Params;
						Params.Volume       = Wanted;
						Params.bLooping     = Current.bLooping;
						Params.bSpatialized = false;
						Params.Bus          = Current.Bus;
						Params.StartFrame   = Current.bResumePlayback ? Current.ResumeFrame : 0;
						Volume->ActiveHandle  = Audio::PlaySound(Current.Sound.Get(), Params).Handle;
						Volume->bPlaying      = Current.ActiveHandle.IsValid();
						Volume->AppliedVolume = Wanted;
					}
					return;
				}

				if (Current.Gain <= 0.0f)
				{
					Volume->StopVoice();
				}
				else if (Math::Abs(Wanted - Current.AppliedVolume) > 0.002f)
				{
					Audio::Context().SetVolume(Current.ActiveHandle, Wanted);
					Volume->AppliedVolume = Wanted;
				}
			});
		}
	}

	bool CAudioSystem::HasWork(const SSystemContext& Context)
	{
		return AnyQueryHasRows(Listeners, Volumes, Sources, ProceduralSources);
	}

	void CAudioSystem::OnTeardown(const SExclusiveContext& Context)
	{

		// No audio device in a headless dedicated server (Audio::Initialize is skipped).
		if (!Audio::HasDevice())
		{
			return;
		}

		// Stop all sounds owned by audio source components in this world.
		auto StopOwnedSound = [](auto Audio)
		{
			if (Audio.Get().bPlaying && Audio.Get().ActiveHandle.IsValid())
			{
				Audio::Context().StopSound(Audio.Get().ActiveHandle);
				Audio->ActiveHandle = SAudioHandle::Invalid();
				Audio->bPlaying = false;
			}
		};
		AllSources.ForEach(StopOwnedSound);
		ProceduralSources.ForEach(StopOwnedSound);

		Volumes.ForEach([](ECS::TMut<SAudioVolumeComponent> Volume)
		{
			const SAudioVolumeComponent& Current = Volume.Get();
			if (Current.bPlaying || Current.ResumeFrame != 0 || Current.Gain != 0.0f)
			{
				Volume->ResumeFrame = 0;
				Volume->StopVoice();
				Volume->Gain = 0.0f;
			}
		});
	}

	void CAudioSystem::OnUpdate(const SSystemContext& Context)
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
			Sources.ForEach([&](ECS::FEntity Entity, ECS::TMut<SAudioSourceComponent> Source, const STransformComponent& Transform)
			{
				const SAudioSourceComponent& Audio = Source.Get();
				const FVector3 Position = Transform.GetWorldLocation();
				Source.SetIfChanged(&SAudioSourceComponent::LastPosition, Position);

				// The mixer may have retired the voice (one-shot ended, evicted by a higher priority).
				if (Audio.bPlaying && Audio::Context().GetVoiceState(Audio.ActiveHandle) == EAudioVoiceState::Free)
				{
					Source->bPlaying = false;
					Source->bPaused = false;
					Source->ActiveHandle = SAudioHandle::Invalid();
				}

				const bool bInRange = !Audio.bSpatialized || !Audio.bCullBeyondMaxDistance || !bHasListener ||
					Math::Distance(Position, ListenerPosition) <= Audio.Attenuation.Resolve().MaxDistance;

				if (!Audio.bReady)
				{
					Source->bReady = true;

					if (Audio.bPlayOnReady && Audio.Sound != nullptr && Audio.Sound->IsPlayable() && bInRange)
					{
						Source->Play();
					}
					return;
				}

				// A looping sound virtualizes out of range; a one shot must not, or it would restart every time it finished.
				if (Audio.bPlayOnReady && Audio.Sound != nullptr && Audio.Sound->IsPlayable() && Audio.IsPersistent())
				{
					if (!Audio.bPlaying && bInRange)
					{
						Source->Play();
					}
					else if (Audio.bPlaying && Audio.bSpatialized && Audio.bCullBeyondMaxDistance && bHasListener &&
						Math::Distance(Position, ListenerPosition) > Audio.Attenuation.Resolve().MaxDistance * 1.1f)
					{
						Source->StopWithMode(EAudioStopMode::Immediate);
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
					Source->bVolumeDirty = false;
				}

				if (Audio.bPitchDirty)
				{
					Audio::Context().SetPitch(Audio.ActiveHandle, Audio.Pitch);
					Source->bPitchDirty = false;
				}

				if (Audio.bLoopingDirty)
				{
					Audio::Context().SetLooping(Audio.ActiveHandle, Audio.bLooping);
					Source->bLoopingDirty = false;
				}

				if (Audio.bAttenuationDirty)
				{
					Audio::Context().SetAttenuation(Audio.ActiveHandle, Audio.Attenuation.Resolve());
					Source->bAttenuationDirty = false;
				}

				if (!Audio.Occlusion.bEnabled || !Audio.bSpatialized || !bOcclusionAllowed)
				{
					return;
				}

				float& TraceTimer = Source.BypassChangeDetection().OcclusionTraceTimer;
				TraceTimer -= DeltaTime;
				if (TraceTimer <= 0.0f && PhysicsScene != nullptr && TraceBudget > 0)
				{
					--TraceBudget;
					TraceTimer = Audio.Occlusion.TraceInterval * (float)Significance.Get(Entity).TickInterval;

					SRayCastSettings Trace;
					Trace.Start     = ListenerPosition;
					Trace.End       = Position;
					Trace.LayerMask = Settings->OcclusionTraceChannel;

					const ECS::FEntity BodyID = Entity;
					if (BodyID != ECS::NullEntity)
					{
						Trace.IgnoreEntities.Add(BodyID);
					}

					Source.SetIfChanged(&SAudioSourceComponent::OcclusionTarget, PhysicsScene->CastRay(Trace).IsSet() ? 1.0f : 0.0f);
				}

				const float PreviousOcclusion = Audio.OcclusionCurrent;
				const float Rate = Audio.Occlusion.InterpTime > 0.0f ? (DeltaTime / Audio.Occlusion.InterpTime) : 1.0f;
				Source.SetIfChanged(&SAudioSourceComponent::OcclusionCurrent, MoveTowards(Audio.OcclusionCurrent, Audio.OcclusionTarget, Rate));

				if (Math::Abs(Audio.OcclusionCurrent - PreviousOcclusion) > 0.001f)
				{
					Audio::Context().SetOcclusion(Audio.ActiveHandle, Audio.OcclusionCurrent,
						Audio.Occlusion.LowPassFrequency, Audio.Occlusion.VolumeAttenuation);
				}
			});
		}

		{
			ProceduralSources.ForEach([&](ECS::FEntity Entity, ECS::TMut<SProceduralAudioComponent> Source)
			{
				const SProceduralAudioComponent& Audio = Source.Get();
				if (!Audio.bReady)
				{
					Source->bReady = true;

					if (Audio.bPlayOnReady)
					{
						Source->Start();
					}
				}

				if (Audio.bPlaying && Audio::Context().GetVoiceState(Audio.ActiveHandle) == EAudioVoiceState::Free)
				{
					Source->bPlaying = false;
					Source->ActiveHandle = SAudioHandle::Invalid();
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
						Source->bVolumeDirty = false;
					}

					if (Audio.bPitchDirty)
					{
						Audio::Context().SetPitch(Audio.ActiveHandle, Audio.Pitch);
						Source->bPitchDirty = false;
					}
				}
			});
		}
	}
}
