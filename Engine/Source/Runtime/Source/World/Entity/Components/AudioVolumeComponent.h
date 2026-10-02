#pragma once

#include "Audio/AudioTypes.h"
#include "Containers/Name.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "AudioVolumeComponent.generated.h"

namespace Lumina
{
	class CSoundBase;

	REFLECT()
	enum class EAudioVolumeShape : uint8
	{
		Box,
		Sphere,
	};

	// A region that plays a sound while the listener is inside it and fades it in and out at the edge.
	REFLECT(Component, Category = "Audio")
	struct RUNTIME_API SAudioVolumeComponent
	{
		GENERATED_BODY()

		PROPERTY(Editable, ToolTip = "Sound that plays while the listener is inside, usually a looping music track or ambience bed.")
		TObjectPtr<CSoundBase> Sound;

		PROPERTY(Editable, ToolTip = "Mix group the zone's sound routes through.")
		EAudioBus Bus = EAudioBus::Ambient;

		PROPERTY(Editable, Category = "Shape")
		EAudioVolumeShape Shape = EAudioVolumeShape::Box;

		PROPERTY(Editable, Category = "Shape", Units = "m", EditCondition = "Shape == Box", EditConditionHides, ToolTip = "Half size of the box in local space, scaled by the entity's transform.")
		FVector3 Extent = FVector3(5.0f);

		PROPERTY(Editable, Category = "Shape", Units = "m", EditCondition = "Shape == Sphere", EditConditionHides, ClampMin = 0.0f, ToolTip = "Sphere radius in local space, scaled by the entity's largest scale axis.")
		float Radius = 5.0f;

		PROPERTY(Editable, Category = "Blending", ToolTip = "Volumes sharing a group are exclusive, so only the highest priority one around the listener plays. Leave empty to layer with every other volume.")
		FName Group;

		PROPERTY(Editable, Category = "Blending", ToolTip = "Wins against overlapping volumes of the same group with a lower priority.")
		int32 Priority = 0;

		PROPERTY(Editable, ClampMin = 0.0f, ClampMax = 4.0f)
		float Volume = 1.0f;

		PROPERTY(Editable, Category = "Blending", ClampMin = 0.0f, Units = "s", ToolTip = "Seconds to rise to full volume after the listener enters or the volume wins its group.")
		float FadeInTime = 2.0f;

		PROPERTY(Editable, Category = "Blending", ClampMin = 0.0f, Units = "s", ToolTip = "Seconds to fall silent after the listener leaves or another volume takes over the group.")
		float FadeOutTime = 2.0f;

		PROPERTY(Editable, Category = "Blending", ClampMin = 0.0f, Units = "m", ToolTip = "Depth inside the boundary over which the sound rises to full volume with distance, or zero for a hard edge.")
		float BlendDistance = 0.0f;

		PROPERTY(Editable, ToolTip = "Restarts the sound when it ends. A one-shot plays once per entry.")
		bool bLooping = true;

		PROPERTY(Editable, ToolTip = "Picks the sound up where it left off when the listener returns, rather than from the start.")
		bool bResumePlayback = false;

		PROPERTY(Editable)
		bool bEnabled = true;

		PROPERTY(ReadOnly, NoSerialize, ScriptReadOnly, Category = "State", ToolTip = "Current fade level from zero to one, before Volume is applied.")
		float Gain = 0.0f;

		PROPERTY(ReadOnly, NoSerialize, ScriptReadOnly, Category = "State")
		bool bListenerInside = false;

		FAudioHandle ActiveHandle;
		float        InsideWeight = 0.0f;
		float        AppliedVolume = -1.0f;
		uint64       ResumeFrame = 0;
		bool         bPlaying = false;
		bool         bFinishedThisVisit = false;

		FUNCTION()
		bool IsListenerInside() const { return bListenerInside; }

		FUNCTION()
		float GetCurrentGain() const { return Gain; }

		// How far inside the listener stands, as a 0 to 1 weight that reaches one at BlendDistance deep.
		float ListenerWeight(const struct STransformComponent& Transform, const FVector3& Listener) const;

		void StopVoice();
	};
}
