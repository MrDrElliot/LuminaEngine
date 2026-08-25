#include "EditorPCH.h"
#include "AudioGraphSchema.h"

#include "AudioGraphPin.h"
#include "Core/Object/Cast.h"
#include "UI/Tools/NodeGraph/EdGraphNode.h"

namespace Lumina
{
    bool FAudioGraphSchema::CanCreateConnection(CEdNodeGraphPin* From, CEdNodeGraphPin* To) const
    {
        if (!FEdGraphSchema::CanCreateConnection(From, To))
        {
            return false;
        }

        CAudioGraphPin* AudioFrom = Cast<CAudioGraphPin>(From);
        CAudioGraphPin* AudioTo   = Cast<CAudioGraphPin>(To);

        // A reroute carries an untyped pin, and the compiler resolves through it to the real producer.
        if (AudioFrom == nullptr || AudioTo == nullptr)
        {
            return true;
        }

        return AudioFrom->GetPinType() == AudioTo->GetPinType();
    }

    const FAudioGraphSchema& GetAudioGraphSchema()
    {
        static FAudioGraphSchema Schema;
        return Schema;
    }
}
