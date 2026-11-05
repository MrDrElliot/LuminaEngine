#pragma once

#include "Containers/HashTable.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Templates/Optional.h"
#include "LifetimeProbeTestTypes.generated.h"

namespace Lumina
{
    // Counts live instances and remembers its own address, so a skipped constructor or destructor, or a bytewise move, is caught.
    struct RUNTIME_API FLifetimeProbe
    {
        FLifetimeProbe() noexcept;
        FLifetimeProbe(const FLifetimeProbe& Other) noexcept;
        FLifetimeProbe(FLifetimeProbe&& Other) noexcept;
        FLifetimeProbe& operator=(const FLifetimeProbe& Other) noexcept;
        FLifetimeProbe& operator=(FLifetimeProbe&& Other) noexcept;
        ~FLifetimeProbe();

        bool operator==(const FLifetimeProbe&) const { return true; }

        NODISCARD static int64 GetLiveCount();
        NODISCARD static int64 GetErrorCount();

    private:

        void Arm();
        void Check(const char* Operation) const;

        const FLifetimeProbe* Self = nullptr;
        uint64 Magic = 0;
    };

    REFLECT()
    struct RUNTIME_API SLifetimeProbeStruct
    {
        GENERATED_BODY()

        PROPERTY(Editable)
        int32 Value = 0;

        PROPERTY(Editable)
        FString Text;

        FLifetimeProbe Probe;
    };

    REFLECT(Component, HideInComponentList)
    struct RUNTIME_API SLifetimeProbeComponent
    {
        GENERATED_BODY()

        PROPERTY(Editable)
        SLifetimeProbeStruct Single;

        PROPERTY(Editable)
        TVector<SLifetimeProbeStruct> List;

        PROPERTY(Editable)
        TOptional<SLifetimeProbeStruct> Maybe;

        PROPERTY(Editable)
        THashMap<int32, SLifetimeProbeStruct> Map;

        PROPERTY(Editable)
        FInstancedStruct Payload;

        PROPERTY(Editable)
        TVector<FInstancedStruct> Payloads;

        FLifetimeProbe Probe;
    };
}
