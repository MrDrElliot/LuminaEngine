#include "EdNodeGraphPin.h"

namespace Lumina
{

    void CEdNodeGraphPin::RemoveConnection(CEdNodeGraphPin* Pin)
    {
        Connections.RemoveSingle(Pin);
    }

    void CEdNodeGraphPin::DisconnectFrom(CEdNodeGraphPin* OtherPin)
    {
        if (!OtherPin)
        {
            return;
        }

        Connections.RemoveSingle(OtherPin);
        OtherPin->Connections.RemoveSingle(this);
    }

    void CEdNodeGraphPin::ClearConnections()
    {
        for (CEdNodeGraphPin* ConnectedPin : Connections)
        {
            ConnectedPin->Connections.RemoveSingle(this);
        }

        Connections.Reset();
    }
}
