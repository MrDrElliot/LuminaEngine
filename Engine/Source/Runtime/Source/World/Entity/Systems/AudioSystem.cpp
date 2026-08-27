#include "RuntimePCH.h"
#include "AudioSystem.h"
#include "World/ECS/Registry.h"
#include "Assets/AssetTypes/Audio/AudioStream.h"
#include "Audio/AudioGlobals.h"
#include "Audio/AudioSettings.h"
#include "Core/Object/ObjectCore.h"
#include "Physics/PhysicsScene.h"
#include "World/Entity/Components/AudioSourceComponent.h"
#include "World/Entity/Components/ProceduralAudioComponent.h"
#include "KinematicsSystem.h"
#include "SignificanceSystem.h"
#include "SystemResources.h"

namespace Lumina
{
	// PhysicsQuery is needed because occlusion casts rays against the live scene.
	FSystemAccess SAudioSystem::Access = FSystemAccess{}
		.Write<SAudioSourceComponent, SProceduralAudioComponent, SAudioListenerComponent>()
		.Read<STransformComponent, SystemResource::PhysicsQuery, SystemResource::Significance,
		      SystemResource::Kinematics>();

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

	void SAudioSystem::Startup(const FSystemContext& Context) noexcept
	{
	}

	void SAudioSystem::Teardown(const FSystemContext& Context) noexcept
	{
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
	}

	void SAudioSystem::Update(const FSystemContext& SystemContext) noexcept
	{
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

		{
			auto ListenerView = SystemContext.CreateView<SAudioListenerComponent>();
			ListenerView.ForEach([&](ECS::FEntity Entity, SAudioListenerComponent& Listener)
			{
				const uint32 Index = (uint32)Math::Clamp(Listener.ListenerIndex, 0, 3);
				const STransformComponent& Transform = XFormStorage.Get(Entity);
				const FVector3 Position = Transform.GetWorldLocation();

				const FVector3 Velocity = Kinematics::GetVelocity(KinematicsState, Entity);

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

					const uint32 BodyID = SystemContext.GetEntityBodyID(Entity);
					if (BodyID != ~0u)
					{
						Trace.IgnoreBodies.push_back(BodyID);
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
