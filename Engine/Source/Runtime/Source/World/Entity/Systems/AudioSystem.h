#pragma once

#include "EntitySystem.h"
#include "AudioSystem.generated.h"

namespace Lumina
{
    REFLECT()
    class RUNTIME_API SAudioSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        // Writes the audio source/procedural components; reads transforms + listeners. The audio device
        // (Audio::Context()) is a process singleton, but this is the only system that touches it within a world,
        // so within-world batching is safe. Defined in the .cpp.

        void OnStartup(const SSystemContext& Context) override;
        void OnUpdate(const SSystemContext& Context) override;
        void OnTeardown(const SSystemContext& Context) override;
    };
}
