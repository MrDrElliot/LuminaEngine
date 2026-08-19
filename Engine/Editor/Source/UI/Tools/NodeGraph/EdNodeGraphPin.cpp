#include "EdNodeGraphPin.h"

namespace Lumina
{

    void CEdNodeGraphPin::RemoveConnection(CEdNodeGraphPin* Pin)
    {
        TVectorRemove(Connections, Pin);
    }

    void CEdNodeGraphPin::DisconnectFrom(CEdNodeGraphPin* OtherPin)
    {
        if (!OtherPin)
        {
            return;
        }

        auto it = std::find(Connections.begin(), Connections.end(), OtherPin);
        if (it != Connections.end())
        {
            Connections.erase(it);
        }

        auto itOther = std::find(OtherPin->Connections.begin(), OtherPin->Connections.end(), this);
        if (itOther != OtherPin->Connections.end())
        {
            OtherPin->Connections.erase(itOther);
        }
    }

    void CEdNodeGraphPin::ClearConnections()
    {
        for (CEdNodeGraphPin* ConnectedPin : Connections)
        {
            auto it = std::find(ConnectedPin->Connections.begin(), ConnectedPin->Connections.end(), this);
            if (it != ConnectedPin->Connections.end())
            {
                ConnectedPin->Connections.erase(it);
            }
        }

        Connections.clear();
    }
}
