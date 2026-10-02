#include "RuntimePCH.h"
#include "AudioVolumeComponent.h"

#include "Audio/AudioGlobals.h"
#include "TransformComponent.h"

namespace Lumina
{
	float SAudioVolumeComponent::ListenerWeight(const STransformComponent& Transform, const FVector3& Listener) const
	{
		const FMatrix4 InvWorld = Math::Inverse(Transform.GetWorldMatrix());
		const FVector3 Local = FVector3(InvWorld * FVector4(Listener, 1.0f));
		const FVector3 Scale = Math::Abs(Transform.GetWorldScale());

		float Depth = 0.0f;
		if (Shape == EAudioVolumeShape::Sphere)
		{
			const float LargestScale = Math::Max(Scale.x, Math::Max(Scale.y, Scale.z));
			Depth = (Radius - Math::Length(Local * Scale / Math::Max(LargestScale, 1e-6f))) * LargestScale;
		}
		else
		{
			const FVector3 Inside = (Extent - Math::Abs(Local)) * Scale;
			Depth = Math::Min(Inside.x, Math::Min(Inside.y, Inside.z));
		}

		if (Depth < 0.0f)
		{
			return 0.0f;
		}
		return BlendDistance > 0.0f ? Math::Min(1.0f, Depth / BlendDistance) : 1.0f;
	}

	void SAudioVolumeComponent::StopVoice()
	{
		if (Audio::HasDevice() && bPlaying && ActiveHandle.IsValid())
		{
			if (bResumePlayback)
			{
				ResumeFrame = Audio::Context().GetPlaybackFrame(ActiveHandle);
			}
			Audio::Context().StopSound(ActiveHandle);
		}
		ActiveHandle = FAudioHandle::Invalid();
		bPlaying = false;
		AppliedVolume = -1.0f;
	}
}
