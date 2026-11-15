#include "RuntimePCH.h"

#include "ObjectReferenceProvider.h"

#include "ObjectArray.h"
#include "Containers/Vector.h"

namespace Lumina
{
    void FObjectReferenceProviders::Register(IObjectReferenceProvider* Provider)
    {
        if (Provider == nullptr)
        {
            return;
        }

        TVector<IObjectReferenceProvider*>& All = Object::Internal::GetReferenceProviders();
        for (IObjectReferenceProvider* Existing : All)
        {
            if (Existing == Provider)
            {
                return;
            }
        }
        All.Add(Provider);
    }

    void FObjectReferenceProviders::Unregister(IObjectReferenceProvider* Provider)
    {
        TVector<IObjectReferenceProvider*>& All = Object::Internal::GetReferenceProviders();
        for (int32 Index = 0; Index < All.Num(); ++Index)
        {
            if (All[Index] == Provider)
            {
                All[Index] = All.Last();
                All.Pop();
                return;
            }
        }
    }

    int32 FObjectReferenceProviders::ForEach(TFunctionRef<void(IObjectReferenceProvider&)> Func)
    {
        int32 Visited = 0;
        for (IObjectReferenceProvider* Provider : Object::Internal::GetReferenceProviders())
        {
            if (Provider != nullptr)
            {
                Func(*Provider);
                ++Visited;
            }
        }
        return Visited;
    }
}
