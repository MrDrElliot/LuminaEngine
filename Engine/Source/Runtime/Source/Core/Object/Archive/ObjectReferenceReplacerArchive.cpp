#include "RuntimePCH.h"
#include "ObjectReferenceReplacerArchive.h"


namespace Lumina
{
    FArchive& FObjectReferenceReplacerArchive::operator<<(CObject*& Obj)
    {
        if (Obj && Obj == ToReplace)
        {
            Obj = Replacement;
        }

        return *this;
    }
}
