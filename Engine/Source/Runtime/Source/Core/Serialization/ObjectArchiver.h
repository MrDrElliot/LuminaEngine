#pragma once
#include "Archiver.h"
#include "ProxyArchive.h"

namespace Lumina
{

    class FObjectArchive : public FArchive
    {
    public:
        
        virtual FArchive& operator<<(CObject*& Value) override
        {
            return *this;
        }
    };
    
    class FObjectProxyArchiver : public FProxyArchive
    {
    public:

        FObjectProxyArchiver(FArchive& InInnerAr, bool bInLoadIfFindFails)
            : FProxyArchive(InInnerAr)
            , bLoadIfFindFails(bInLoadIfFindFails)
        {
        }

        // Declaring ANY operator<< here hides every one of the base's overloads, so without this a plain
        // `Ar << SomeInt` against this archiver does not compile at all -- the scalar, string and container
        // forwards are all invisible. Overriding two of them is not meant to withdraw the other twenty.
        using FProxyArchive::operator<<;

        RUNTIME_API FArchive& operator<<(CObject*& Obj) override;
        RUNTIME_API FArchive& operator<<(FObjectHandle& Value) override;


    private:

        bool bLoadIfFindFails =false;
    };

    // FObjectProxyArchiver that rewrites resolved references through a source -> copy table.
    //
    // Object references serialize as GUIDs, so a plain property copy hands the destination the SOURCE
    // objects: duplicate a package whose exports reference each other (a material and the node graph
    // beside it, a graph and its nodes) and the copy points straight back into the original. Editing
    // the duplicate then edits the original. Remapping on read is what makes a package-level duplicate
    // self-contained; anything not in the table is left alone, which is exactly right for references to
    // OTHER packages (textures, parent materials) that should stay shared.
    class FObjectRemapArchiver : public FObjectProxyArchiver
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
            const auto Itr = Remap.find(Source);
            return Itr != Remap.end() ? Itr->second : Source;
        }

        const THashMap<CObject*, CObject*>& Remap;
    };
}
