#pragma once

#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectReferenceProvider.h"
#include "Core/Reflection/Type/ObjectReferenceVisitor.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CClass;
    class CObject;

    struct FReinstanceResult
    {
        int32 InstancesReplaced = 0;
        int32 ReferencesPatched = 0;
        int32 ObjectsScanned = 0;
        int32 ProvidersVisited = 0;

        // Originals still strongly referenced after the swap, so they retire on their own refcount.
        int32 OriginalsOutlivingTheSwap = 0;

        // Every original and its replacement, classes included, for whoever has to rebind something the walk could not reach.
        THashMap<CObject*, CObject*> Replaced;
    };

    // Swaps live instances onto a new class keeping name, GUID, package and flags, and repoints every reference.
    class RUNTIME_API FObjectReinstancer
    {
    public:

        // Every instance of Old becomes an instance of New.
        void MapClass(CClass* Old, CClass* New);

        using FPostReplaceFunc = void (*)(CObject* Old, CObject* New);

        // Runs on each replacement after its values are carried over and before anything is repointed at it.
        void SetPostReplaceHook(FPostReplaceFunc Func) { PostReplace = Func; }

        NODISCARD bool IsEmpty() const { return ClassMap.IsEmpty(); }

        // Builds the replacements, repoints the graph, and retires the originals.
        FReinstanceResult Commit();

    private:

        CObject* BuildReplacement(CObject* Old, CClass* NewClass);

        THashMap<CClass*, CClass*>   ClassMap;
        THashMap<CObject*, CObject*> ObjectMap;

        FPostReplaceFunc PostReplace = nullptr;
    };
}
