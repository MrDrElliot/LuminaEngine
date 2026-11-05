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

        /** Originals still strongly referenced after the swap, so they retire on their own refcount. */
        int32 OriginalsOutlivingTheSwap = 0;

        // Every original and its replacement, classes included, for whoever has to rebind something the walk could not reach.
        THashMap<CObject*, CObject*> Replaced;
    };

    /**
     * Moves every live instance of a class onto a replacement class and repoints everything that pointed at
     * the old ones, so a type can change shape underneath a running program.
     *
     * Identity is preserved rather than rebuilt: an instance keeps its name, GUID, package and flags, and
     * every reference to it ends up on the replacement. Holders therefore need to know nothing, which is the
     * difference between this and serializing each known holder out and back by hand.
     */
    class RUNTIME_API FObjectReinstancer
    {
    public:

        /** Every instance of Old becomes an instance of New. */
        void MapClass(CClass* Old, CClass* New);

        using FPostReplaceFunc = void (*)(CObject* Old, CObject* New);

        /** Runs on each replacement once its values are carried over, before anything is repointed at it.
         *  The seam for a policy the reinstancer itself has no business knowing, such as which properties a
         *  language wants reset to their new default rather than migrated. */
        void SetPostReplaceHook(FPostReplaceFunc Func) { PostReplace = Func; }

        NODISCARD bool IsEmpty() const { return ClassMap.IsEmpty(); }

        /** Builds the replacements, repoints the graph, and retires the originals. */
        FReinstanceResult Commit();

    private:

        CObject* BuildReplacement(CObject* Old, CClass* NewClass);

        THashMap<CClass*, CClass*>   ClassMap;
        THashMap<CObject*, CObject*> ObjectMap;

        FPostReplaceFunc PostReplace = nullptr;
    };
}
