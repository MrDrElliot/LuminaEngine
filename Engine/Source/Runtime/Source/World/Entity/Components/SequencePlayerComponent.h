#pragma once

#include "Assets/AssetTypes/Sequence/Sequence.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "SequencePlayerComponent.generated.h"

namespace Lumina
{
    REFLECT()
    enum class RUNTIME_API ESequenceFinishAction : uint8
    {
        // Leave the last evaluated pose in place; the shot holds on its final frame.
        Hold,
        // Put possessed entities back where they started and destroy spawned ones.
        Restore,
    };

    // Plays a CSequence against the world. Driven entirely by its own fields, so script starts a cutscene
    // by setting bPlaying rather than calling into anything.
    REFLECT(Component, Category = "Cinematics")
    struct RUNTIME_API SSequencePlayerComponent
    {
        GENERATED_BODY()

        PROPERTY(Script, Editable, Category = "Sequence")
        TObjectPtr<CSequence> Sequence;

        /** Begins playing as soon as the component exists, for cutscenes triggered by level load. */
        PROPERTY(Script, Editable, Category = "Sequence")
        bool bAutoPlay = false;

        /** Set true to play, false to stop. The system watches the transition and does the binding, the
         *  spawning, and the teardown, so nothing else has to be called. */
        PROPERTY(Script, Editable, Category = "Sequence")
        bool bPlaying = false;

        PROPERTY(Script, Editable, Category = "Sequence")
        bool bLoop = false;

        PROPERTY(Script, Editable, ClampMin = 0.0f, Category = "Sequence")
        float PlayRate = 1.0f;

        /** What happens to the driven entities when playback stops or reaches the end. */
        PROPERTY(Script, Editable, Category = "Sequence")
        ESequenceFinishAction FinishAction = ESequenceFinishAction::Restore;

        /** Current playhead in seconds. Writable so script can scrub or restart. */
        PROPERTY(Script, Editable, Category = "Sequence")
        float Time = 0.0f;

        /** Live bindings, spawned entities and the restore snapshot. Rebuilt on play; never serialized. */
        FSequenceInstance Instance;

        /** Sequence the instance was bound against, so the system notices the asset being swapped. */
        const CSequence* BoundSequence = nullptr;

        /** Playback was started or the time jumped, so latching tracks re-apply. */
        bool bJumped = false;

        /** Previous frame's playhead, for tracks that need the interval rather than the instant. */
        float PreviousTime = 0.0f;
    };
}
