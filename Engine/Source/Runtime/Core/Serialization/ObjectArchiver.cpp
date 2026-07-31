#include "pch.h"
#include "ObjectArchiver.h"

#include "Core/Object/Object.h"
#include "Core/Object/ObjectCore.h"

namespace Lumina
{
    FArchive& FObjectProxyArchiver::operator<<(CObject*& Obj)
    {
        if (IsWriting())
        {
            if (Obj)
            {
                FGuid GUID = Obj->GetGUID();
                InnerArchive << GUID;
            }
            else
            {
                FGuid GUID = FGuid::Invalid();
                InnerArchive << GUID;
            }
        }
        else if (IsReading())
        {
            FGuid GUID;
            InnerArchive << GUID;

            if (!GUID.IsValid())
            {
                Obj = nullptr;
                return *this;
            }

            Obj = FindObject<CObject>(GUID);

            if (!Obj && bLoadIfFindFails)
            {
                Obj = LoadObject<CObject>(GUID);
            }   
        }

        return *this;
    }

    FArchive& FObjectRemapArchiver::operator<<(CObject*& Obj)
    {
        FObjectProxyArchiver::operator<<(Obj);

        // Write side is untouched: the source object's GUID is what goes into the buffer. The swap
        // happens on the way back out, once the GUID has resolved to the object it named.
        if (IsReading())
        {
            Obj = Remapped(Obj);
        }

        return *this;
    }

    FArchive& FObjectRemapArchiver::operator<<(FObjectHandle& Value)
    {
        FObjectProxyArchiver::operator<<(Value);

        if (IsReading() && Value.IsValid())
        {
            if (CObject* Mapped = Remapped(Value.Resolve()))
            {
                Value = Mapped;
            }
        }

        return *this;
    }

    FArchive& FObjectProxyArchiver::operator<<(FObjectHandle& Value)
    {
        if (IsWriting())
        {
            if (Value.IsValid())
            {
                CObject* ResolvedObject = Value.Resolve();
                
                FGuid GUID = ResolvedObject->GetGUID();
                InnerArchive << GUID;
            }
            else
            {
                FGuid GUID = FGuid::Invalid();
                InnerArchive << GUID;
            }
        }
        else if (IsReading())
        {
            FGuid GUID;
            InnerArchive << GUID;
            
            if (!GUID.IsValid())
            {
                Value = nullptr;
                return *this;
            }

            Value = FindObject<CObject>(GUID);

            if (!Value.IsValid() && bLoadIfFindFails)
            {
                Value = LoadObject<CObject>(GUID);
            }   
        }

        return *this;
    }
}
