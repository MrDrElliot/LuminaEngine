#pragma once
#include "Archiver.h"
#include "ProxyArchive.h"

namespace Lumina
{

    class FObjectArchive : public FArchive
    {
    public:

        using FArchive::operator<<;

        virtual FArchive& operator<<(CObject*& Value) override
        {
            return *this;
        }
    };
    
    class LUMINA_VISIBLE_TYPE FObjectProxyArchiver : public FProxyArchive
    {
    public:

        FObjectProxyArchiver(FArchive& InInnerAr, bool bInLoadIfFindFails)
            : FProxyArchive(InInnerAr)
            , bLoadIfFindFails(bInLoadIfFindFails)
        {
        }

        // Declaring any operator<< here hides every base overload, so the using-declaration below keeps the rest visible.
        using FProxyArchive::operator<<;

        RUNTIME_API FArchive& operator<<(CObject*& Obj) override;
        RUNTIME_API FArchive& operator<<(FObjectHandle& Value) override;


    private:

        bool bLoadIfFindFails =false;
    };

    // Names travel as their ids, which only the writing process can read back, for a copy made in memory.
    class LUMINA_VISIBLE_TYPE FInProcessObjectArchiver : public FObjectProxyArchiver
    {
    public:

        explicit FInProcessObjectArchiver(FArchive& InInnerAr)
            : FObjectProxyArchiver(InInnerAr, /*bLoadIfFindFails*/ true)
        {
        }

        using FObjectProxyArchiver::operator<<;

        FArchive& operator<<(FName& Value) override
        {
            static_assert(sizeof(FName) == 2 * sizeof(uint32) && std::is_trivially_copyable_v<FName>);
            Serialize(&Value, sizeof(FName));
            return *this;
        }
    };

    // Rewrites resolved references through a source to copy table so a duplicated package does not point back into the original. References not in the table stay shared.
    class LUMINA_VISIBLE_TYPE FObjectRemapArchiver : public FObjectProxyArchiver
    {
    public:

        FObjectRemapArchiver(FArchive& InInnerAr, const THashMap<CObject*, CObject*>& InRemap)
            : FObjectProxyArchiver(InInnerAr, /*bLoadIfFindFails*/ true)
            , Remap(InRemap)
        {
        }

        // Same hiding rule one level down; see FObjectProxyArchiver.
        using FObjectProxyArchiver::operator<<;

        RUNTIME_API FArchive& operator<<(CObject*& Obj) override;
        RUNTIME_API FArchive& operator<<(FObjectHandle& Value) override;

    private:

        CObject* Remapped(CObject* Source) const
        {
            const auto Itr = Remap.Find(Source);
            return Itr != nullptr ? (*Itr) : Source;
        }

        const THashMap<CObject*, CObject*>& Remap;
    };
}
