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
        // Several loading phases run AFTER the editor UI is up, so a stale palette would persist.
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

        // GetDefaultObject allocates into GObjectArray, so the walk collects before it resolves.
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

            // Family bases and graph-created root nodes, which is class metadata rather than a virtual.
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
