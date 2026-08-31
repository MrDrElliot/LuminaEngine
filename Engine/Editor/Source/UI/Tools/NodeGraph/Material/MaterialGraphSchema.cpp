#include "MaterialGraphSchema.h"

#include "MaterialInput.h"
#include "MaterialOutput.h"
#include "Core/Object/Cast.h"
#include "UI/Tools/NodeGraph/EdGraphNode.h"

namespace Lumina
{
    namespace
    {
        // Float for a non-material pin, the permissive answer that keeps unknown pins connecting freely.
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
            return Owner != nullptr && Owner->IsUntypedPassthrough();
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
