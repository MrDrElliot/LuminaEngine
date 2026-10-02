#include "RuntimePCH.h"
#include "AudioSystem.h"
#include "World/ECS/Registry.h"
#include "Assets/AssetTypes/Audio/AudioStream.h"
#include "Audio/AudioGlobals.h"
#include "Audio/AudioSettings.h"
#include "Audio/SoundPlayback.h"
#include "Core/Object/ObjectCore.h"
#include "Physics/PhysicsScene.h"
#include "World/Entity/Components/AudioSourceComponent.h"
#include "World/Entity/Components/AudioVolumeComponent.h"
#include "World/Entity/Components/ProceduralAudioComponent.h"
#include "KinematicsSystem.h"
#include "SignificanceSystem.h"
#include "SystemResources.h"
#include "World/World.h"

namespace Lumina
{
	// PhysicsQuery is needed because occlusion casts rays against the live scene.
    void SAudioSystem::Configure()
    {
        RequireUpdate(EUpdateStage::PostPhysics);
        Writes<SAudioSourceComponent, SProceduralAudioComponent, SAudioListenerComponent, SAudioVolumeComponent>();
        Reads<STransformComponent, SystemResource::PhysicsQuery, SystemResource::Significance, SystemResource::Kinematics>();
    }

	namespace
	{
		float MoveTowards(float Current, float Target, float MaxDelta)
		{
			if (Math::Abs(Target - Current) <= MaxDelta)
			{
				return Target;
			}
			return Current + (Target > Current ? MaxDelta : -MaxDelta);
		}
	}

	namespace
	{
		struct FVolumeGroupWinner
		{
			FName        Group;
			ECS::FEntity Entity = ECS::NullEntity;
			int32        Priority = 0;
			float        Weight = 0.0f;
		};

		// Each volume fades toward its own weight when it is ungrouped or wins its group, and toward silence otherwise.
		template <typename TTransformStorage>
		void UpdateAudioVolumes(const FSystemContext& Context, TTransformStorage& XForms, const FVector3& Listener, float DeltaTime)
		{
			TVector<FVolumeGroupWinner> Winners;
			TVector<SAudioVolumeComponent*> GroupVoices;
			auto View = Context.CreateView<SAudioVolumeComponent>();
			View.ForEach([&](ECS::FEntity Entity, SAudioVolumeComponent& Volume)
			{
				if (Volume.bPlaying && !Volume.Group.IsNone())
				{
					GroupVoices.push_back(&Volume);
				}

				Volume.InsideWeight = (Volume.bEnabled && XForms.Contains(Entity)) ? Volume.ListenerWeight(XForms.Get(Entity), Listener) : 0.0f;
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
					Winners.push_back(FVolumeGroupWinner{ Volume.Group, Entity, Volume.Priority, Volume.InsideWeight });
				}
				else if (Volume.Priority > Winner->Priority || (Volume.Priority == Winner->Priority && Volume.InsideWeight > Winner->Weight))
				{
					*Winner = FVolumeGroupWinner{ Volume.Group, Entity, Volume.Priority, Volume.InsideWeight };
				}
			});

			View.ForEach([&](ECS::FEntity Entity, SAudioVolumeComponent& Volume)
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
					Volume.ActiveHandle = FAudioHandle::Invalid();
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
							Other->ActiveHandle  = FAudioHandle::Invalid();
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
						FAudioPlayParams Params;
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

	void SAudioSystem::OnStartup()
	{
	}

	void SAudioSystem::OnTeardown()
	{
	    const FSystemContext& Context = GetContext();

		// No audio device in a headless dedicated server (Audio::Initialize is skipped).
		if (!Audio::HasDevice())
		{
			return;
		}

		// Stop all sounds owned by audio source components in this world.
		auto View = Context.CreateView<SAudioSourceComponent>();
		View.ForEach([](SAudioSourceComponent& Audio)
		{
			if (Audio.bPlaying && Audio.ActiveHandle.IsValid())
			{
				Audio::Context().StopSound(Audio.ActiveHandle);
				Audio.ActiveHandle = FAudioHandle::Invalid();
				Audio.bPlaying = false;
			}
		});

		auto ProceduralView = Context.CreateView<SProceduralAudioComponent>();
		ProceduralView.ForEach([](SProceduralAudioComponent& Audio)
		{
			if (Audio.bPlaying && Audio.ActiveHandle.IsValid())
			{
				Audio::Context().StopSound(Audio.ActiveHandle);
				Audio.ActiveHandle = FAudioHandle::Invalid();
				Audio.bPlaying = false;
			}
		});

		auto VolumeView = Context.CreateView<SAudioVolumeComponent>();
		VolumeView.ForEach([](SAudioVolumeComponent& Volume)
		{
			Volume.ResumeFrame = 0;
			Volume.StopVoice();
			Volume.Gain = 0.0f;
		});
	}

	void SAudioSystem::OnUpdate()
	{
	    const FSystemContext& SystemContext = GetContext();

		LUMINA_PROFILE_SCOPE();

		// No audio device in a headless dedicated server (Audio::Initialize is skipped).
		if (!Audio::HasDevice())
		{
			return;
		}

		const CAudioSettings* Settings = GetDefault<CAudioSettings>();
		const float DeltaTime = (float)SystemContext.GetDeltaTime();

		auto XFormStorage = SystemContext.GetStorage<STransformComponent>();
		const FKinematicsState* KinematicsState = Kinematics::GetState(SystemContext);

		FVector3 ListenerPosition(0.0f);
		bool bHasListener = false;
		uint32 DrivenListenerMask = 0;

		CWorld* World = SystemContext.GetWorld();
		const ECS::FEntity ActiveCamera = World != nullptr ? World->GetActiveCameraEntity() : ECS::NullEntity;
		const bool bCameraHasTransform = ActiveCamera != ECS::NullEntity && !ActiveCamera.IsTombstone() && XFormStorage.Contains(ActiveCamera);

		{
			auto ListenerView = SystemContext.CreateView<SAudioListenerComponent>();
			ListenerView.ForEach([&](ECS::FEntity Entity, SAudioListenerComponent& Listener)
			{
				const uint32 Index = (uint32)Math::Clamp(Listener.ListenerIndex, 0, 3);
				const ECS::FEntity Ears = (Listener.bFollowActiveCamera && Index == 0 && bCameraHasTransform) ? ActiveCamera : Entity;
				const STransformComponent& Transform = XFormStorage.Get(Ears);
				const FVector3 Position = Transform.GetWorldLocation();

				const FVector3 Velocity = Kinematics::GetVelocity(KinematicsState, Ears);

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
			UpdateAudioVolumes(SystemContext, XFormStorage, ListenerPosition, DeltaTime);
		}

		const bool bOcclusionAllowed = bHasListener && Settings != nullptr && Settings->bOcclusionEnabled;
		Physics::IPhysicsScene* PhysicsScene = bOcclusionAllowed ? SystemContext.GetPhysicsScene() : nullptr;
		uint32 TraceBudget = (Settings != nullptr) ? Settings->MaxOcclusionTracesPerTick : 0;
		const FSignificanceState* SignificanceState = Significance::GetState(SystemContext);

		{
			auto SourceView = SystemContext.CreateView<SAudioSourceComponent>();
			SourceView.ForEach([&](ECS::FEntity Entity, SAudioSourceComponent& Audio)
			{
				const STransformComponent& Transform = XFormStorage.Get(Entity);
				const FVector3 Position = Transform.GetWorldLocation();
				Audio.LastPosition = Position;

				// The mixer may have retired the voice (one-shot ended, evicted by a higher priority).
				if (Audio.bPlaying && Audio::Context().GetVoiceState(Audio.ActiveHandle) == EAudioVoiceState::Free)
				{
					Audio.bPlaying = false;
					Audio.bPaused = false;
					Audio.ActiveHandle = FAudioHandle::Invalid();
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

				// A sound that plays until stopped virtualizes, dropping its voice out of range and taking
				// a new one back. A one shot must not, or it would restart every time it finished.
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

				const FVector3 Velocity = Kinematics::GetVelocity(KinematicsState, Entity);

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
					Audio.OcclusionTraceTimer = Significance::ScaleInterval(SignificanceState, Entity, Audio.Occlusion.TraceInterval);

					SRayCastSettings Trace;
					Trace.Start     = ListenerPosition;
					Trace.End       = Position;
					Trace.LayerMask = Settings->OcclusionTraceChannel;

					const ECS::FEntity BodyID = Entity;
					if (BodyID != ECS::NullEntity)
					{
						Trace.IgnoreEntities.push_back(BodyID);
					}

					Audio.OcclusionTarget = PhysicsScene->CastRay(Trace).has_value() ? 1.0f : 0.0f;
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
			auto ProceduralView = SystemContext.CreateView<SProceduralAudioComponent>();
			ProceduralView.ForEach([&](ECS::FEntity Entity, SProceduralAudioComponent& Audio)
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
					Audio.ActiveHandle = FAudioHandle::Invalid();
				}

				if (Audio.bPlaying && Audio.ActiveHandle.IsValid())
				{
					if (Audio.bSpatialized)
					{
						const STransformComponent& Transform = XFormStorage.Get(Entity);
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
