#pragma once

#include "Audio/Graph/AudioGraphTypes.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "UI/Tools/NodeGraph/EdNodeGraphPin.h"
#include "AudioGraphPin.generated.h"

namespace Lumina
{
    class CObject;

    /** Wire color for a value kind, so a graph reads by type at a glance. */
    EDITOR_API uint32 GetAudioGraphTypeColor(EAudioGraphType Type);

    REFLECT()
    class CAudioGraphPin : public CEdNodeGraphPin
    {
        GENERATED_BODY()

    public:

        float DrawPin() override;
        bool HasInlineEditor() const override;

        uint32 GetPinColor() const override { return GetAudioGraphTypeColor(PinType); }

        void SetPinType(EAudioGraphType InType) { PinType = InType; }
        EAudioGraphType GetPinType() const { return PinType; }

        /** Reflected property on the owning node holding this pin's value while no wire drives it. */
        void BindDefaultProperty(FProperty* Property) { DefaultProperty = Property; }
        FProperty* GetDefaultProperty() const { return DefaultProperty; }

        //~ Value the compiler bakes in for an unconnected pin. Zero when the pin binds no property.
        float ReadFloatDefault() const;
        int32 ReadIntDefault() const;
        bool ReadBoolDefault() const;
        CObject* ReadObjectDefault() const;

    private:

        /** Address of the bound value on the owning node, or null when nothing is bound. */
        void* GetDefaultValuePtr() const;

        EAudioGraphType PinType = EAudioGraphType::Float;
        FProperty*      DefaultProperty = nullptr;
    };
}
