#include "MaterialGraphSchema.h"

#include "MaterialInput.h"
#include "MaterialOutput.h"
#include "Core/Object/Cast.h"
#include "UI/Tools/NodeGraph/EdGraphNode.h"

namespace Lumina
{
    namespace
    {
        // The pin's declared material type, or Float for anything that isn't a material pin (which is
        // the permissive answer: unknown pins keep the old behavior of connecting to anything).
        EMaterialInputType PinInputType(CEdNodeGraphPin* Pin)
        {
            if (CMaterialInput* In = Cast<CMaterialInput>(Pin))
            {
                return In->GetInputType();
            }
            if (CMaterialOutput* Out = Cast<CMaterialOutput>(Pin))
            {
                return Out->InputType;
            }
            return EMaterialInputType::Float;
        }

        bool IsRerouteEndpoint(CEdNodeGraphPin* Pin)
        {
            CEdGraphNode* Owner = Pin != nullptr ? Pin->GetOwningNode() : nullptr;
            return Owner != nullptr && Owner->IsRerouteNode();
        }
    }

    bool FMaterialGraphSchema::CanCreateConnection(CEdNodeGraphPin* From, CEdNodeGraphPin* To) const
    {
        if (!FEdGraphSchema::CanCreateConnection(From, To))
        {
            return false;
        }

        if (IsRerouteEndpoint(From) || IsRerouteEndpoint(To))
        {
            return true;
        }

        const bool bFromHandle = PinInputType(From) == EMaterialInputType::TextureHandle;
        const bool bToHandle   = PinInputType(To)   == EMaterialInputType::TextureHandle;

        return bFromHandle == bToHandle;
    }

    const FMaterialGraphSchema& GetMaterialGraphSchema()
    {
        static FMaterialGraphSchema Schema;
        return Schema;
    }
}
