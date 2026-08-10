#include "GraphNodeRegistry.h"

#include "EdGraphNode.h"
#include "Core/Delegates/CoreDelegates.h"
#include "Core/Object/ObjectArray.h"

namespace Lumina
{
    FGraphNodeRegistry& FGraphNodeRegistry::Get()
    {
        static FGraphNodeRegistry Registry;
        return Registry;
    }

    FGraphNodeRegistry::FGraphNodeRegistry()
    {
        // Plugin and project DLLs register their reflected classes when they load, and several loading
        // phases run AFTER the editor UI is up. Without this, a graph opened before the plugin loaded
        // would keep serving a stale palette for the rest of the session.
        (void)FCoreDelegates::OnModuleLoaded.AddLambda([this](FModuleInfo*)
        {
            Invalidate();
        });
    }

    const THashSet<CClass*>& FGraphNodeRegistry::GetNodesForGraphClass(CClass* GraphClass)
    {
        auto Existing = Cache.find(GraphClass);
        if (Existing != Cache.end())
        {
            return Existing->second;
        }

        return Cache.emplace(GraphClass, BuildForGraphClass(GraphClass)).first->second;
    }

    THashSet<CClass*> FGraphNodeRegistry::BuildForGraphClass(CClass* GraphClass) const
    {
        CClass* NodeBase = CEdGraphNode::StaticClass();

        // Two passes on purpose: GetDefaultObject() allocates into GObjectArray, and mutating the array
        // from inside its own walk is what this collect-then-resolve shape exists to avoid.
        TVector<CClass*> Candidates;
        GObjectArray.ForEachObject([&](CObjectBase* Object, int32)
        {
            if (Object == nullptr || !Object->IsA<CClass>())
            {
                return;
            }

            CClass* Class = static_cast<CClass*>(Object);

            // IsChildOf is reflexive, so the base itself has to be excluded by hand.
            if (Class == NodeBase || !Class->IsChildOf(NodeBase))
            {
                return;
            }

            // Family bases and graph-created root nodes. See the note on FGraphNodeRegistry for why
            // this is class metadata and not a virtual.
            if (Class->HasMeta("NotPlaceable"))
            {
                return;
            }

            Candidates.push_back(Class);
        });

        THashSet<CClass*> Supported;
        for (CClass* Class : Candidates)
        {
            CEdGraphNode* CDO = Class->GetDefaultObject<CEdGraphNode>();
            if (CDO != nullptr && CDO->IsSupportedInGraph(GraphClass))
            {
                Supported.emplace(Class);
            }
        }

        return Supported;
    }
}
