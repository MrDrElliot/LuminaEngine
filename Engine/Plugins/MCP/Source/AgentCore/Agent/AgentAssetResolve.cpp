#include "AgentCorePCH.h"
#include "Agent/AgentAssetResolve.h"

#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Containers/StringFormat.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectIterator.h"
#include "GUID/GUID.h"

namespace Lumina::Agent
{
    TOptional<FGuid> ParseAssetGuid(FStringView GuidOrPath)
    {
        TOptional<FGuid> Parsed = FGuid::TryParse(GuidOrPath);
        if (!Parsed.IsSet())
        {
            if (const FAssetData* Data = FAssetRegistry::Get().GetAssetByPath(GuidOrPath))
            {
                Parsed = Data->AssetGUID;
            }
        }
        return Parsed;
    }

    bool ResolveAssetObject(FStringView GuidOrPath, CObject*& OutObject, FString& OutError)
    {
        const TOptional<FGuid> Parsed = ParseAssetGuid(GuidOrPath);
        if (!Parsed.IsSet())
        {
            OutError = Lumina::Format("'{}' is neither a GUID nor the content path of an asset.", GuidOrPath);
            return false;
        }

        OutObject = StaticLoadObject(*Parsed);
        if (OutObject == nullptr)
        {
            OutError = Lumina::Format("No asset {} could be loaded.", GuidOrPath);
            return false;
        }

        return true;
    }

    CClass* FindClassByName(FStringView Name, CClass* Base, FString& OutError)
    {
        if (Name.empty())
        {
            OutError = "A class name is needed.";
            return nullptr;
        }

        CClass* Class = FindObject<CClass>(FName(Name));

        // Script classes are named Namespace.Class, so a bare class name finds the one script class that ends with it.
        if (Class == nullptr)
        {
            const FString Suffix = FString(".") + FString(Name.data(), Name.size());
            int32 Matches = 0;
            for (TObjectIterator<CClass> It; It; ++It)
            {
                const FString Candidate = (*It)->GetName().ToString();
                if (Candidate.size() > Suffix.size() && Candidate.compare(Candidate.size() - Suffix.size(), Suffix.size(), Suffix) == 0)
                {
                    Class = *It;
                    ++Matches;
                }
            }
            if (Matches > 1)
            {
                OutError = Lumina::Format("More than one script class is called {}, so give its namespace too.", Name);
                return nullptr;
            }
        }

        if (Class == nullptr)
        {
            OutError = Lumina::Format("No class is called {}.", Name);
            return nullptr;
        }

        if (Base != nullptr && !Class->IsChildOf(Base))
        {
            OutError = Lumina::Format("{} does not derive from {}.", Name, Base->GetName());
            return nullptr;
        }

        return Class;
    }
}
