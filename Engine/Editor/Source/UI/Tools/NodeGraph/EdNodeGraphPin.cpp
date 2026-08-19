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

        auto it = Algo::Find(Connections.begin(), Connections.end(), OtherPin);
        if (it != Connections.end())
        {
            Connections.erase(it);
        }

        auto itOther = Algo::Find(OtherPin->Connections.begin(), OtherPin->Connections.end(), this);
        if (itOther != OtherPin->Connections.end())
        {
            OtherPin->Connections.erase(itOther);
        }
    }

    void CEdNodeGraphPin::ClearConnections()
    {
        for (CEdNodeGraphPin* ConnectedPin : Connections)
        {
            auto it = Algo::Find(ConnectedPin->Connections.begin(), ConnectedPin->Connections.end(), this);
            if (it != ConnectedPin->Connections.end())
            {
                ConnectedPin->Connections.erase(it);
            }
        }

        Connections.clear();
    }
}
