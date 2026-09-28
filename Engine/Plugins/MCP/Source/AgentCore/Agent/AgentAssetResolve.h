#pragma once

#include "Containers/String.h"
#include "Containers/StringFormat.h"
#include "Containers/StringView.h"
#include "Core/Object/Class.h"
#include "Core/Object/Object.h"
#include "GUID/GUID.h"

namespace Lumina::Agent
{
    // The GUID a GUID string or registered content path names, unset when it is neither.
    NODISCARD AGENTCORE_API TOptional<FGuid> ParseAssetGuid(FStringView GuidOrPath);

    // Loads the asset a GUID or content path names, such as /Game/Content/Decals/T_Scorch.
    NODISCARD AGENTCORE_API bool ResolveAssetObject(FStringView GuidOrPath, CObject*& OutObject, FString& OutError);

    template<typename T>
    NODISCARD bool ResolveAsset(FStringView Guid, T*& OutAsset, FString& OutError)
    {
        CObject* Object = nullptr;
        if (!ResolveAssetObject(Guid, Object, OutError))
        {
            return false;
        }

        if (!Object->IsA<T>())
        {
            OutError = Lumina::Format("'{}' is a {}, not a {}.", Guid, Object->GetClass()->GetName(), T::StaticClass()->GetName());
            return false;
        }

        OutAsset = static_cast<T*>(Object);
        return true;
    }

    // Finds a reflected class by name, optionally requiring it to derive from Base.
    NODISCARD AGENTCORE_API CClass* FindClassByName(FStringView Name, CClass* Base, FString& OutError);
}
