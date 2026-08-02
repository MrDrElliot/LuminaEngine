#pragma once

#include "EdGraphNode.h"
#include "EdNodeGraphPin.h"
#include "Containers/Array.h"

namespace Lumina::GraphAlgorithms
{
    // Kahn's algorithm over a pre-computed reachable set. Writes SortedNodes in dependency order
    // (roots-last). Returns the first node participating in a cycle, or nullptr on success.
    inline CEdGraphNode* TopologicalSortReachable(const TVector<TObjectPtr<CEdGraphNode>>& Nodes, const THashSet<CEdGraphNode*>& ReachableNodes, TVector<CEdGraphNode*>& SortedNodes)
    {
        THashMap<CEdGraphNode*, uint32> InDegree;
        for (CEdGraphNode* Node : Nodes)
        {
            if (ReachableNodes.find(Node) != ReachableNodes.end())
            {
                InDegree[Node] = 0;
            }
        }

        // Wireless edges (a named reroute usage depending on its declaration) carry no pin connection,
        // so they are gathered up front and counted exactly like real ones. Doing it here rather than
        // special-casing the emit order also means a usage placed above its own declaration is
        // reported as an ordinary cycle instead of silently emitting in the wrong order.
        THashMap<CEdGraphNode*, TVector<CEdGraphNode*>> ImplicitDependents;
        for (CEdGraphNode* Node : Nodes)
        {
            if (ReachableNodes.find(Node) == ReachableNodes.end())
            {
                continue;
            }

            CEdGraphNode* Implicit = Node->GetImplicitInputNode();
            if (Implicit != nullptr && Implicit != Node && ReachableNodes.find(Implicit) != ReachableNodes.end())
            {
                ImplicitDependents[Implicit].push_back(Node);
                InDegree[Node]++;
            }
        }

        for (CEdGraphNode* Node : Nodes)
        {
            if (ReachableNodes.find(Node) == ReachableNodes.end())
            {
                continue;
            }

            for (CEdNodeGraphPin* OutputPin : Node->GetOutputPins())
            {
                for (CEdNodeGraphPin* ConnectedPin : OutputPin->GetConnections())
                {
                    CEdGraphNode* ConnectedNode = ConnectedPin->GetOwningNode();
                    if (ReachableNodes.find(ConnectedNode) != ReachableNodes.end())
                    {
                        InDegree[ConnectedNode]++;
                    }
                }
            }
        }

        TQueue<CEdGraphNode*> ReadyQueue;
        for (auto& Pair : InDegree)
        {
            if (Pair.second == 0)
            {
                ReadyQueue.push(Pair.first);
            }
        }

        uint32 ProcessedNodeCount = 0;
        while (!ReadyQueue.empty())
        {
            CEdGraphNode* Node = ReadyQueue.front();
            ReadyQueue.pop();
            SortedNodes.push_back(Node);
            ProcessedNodeCount++;

            for (CEdNodeGraphPin* OutputPin : Node->GetOutputPins())
            {
                for (CEdNodeGraphPin* ConnectedPin : OutputPin->GetConnections())
                {
                    CEdGraphNode* ConnectedNode = ConnectedPin->GetOwningNode();
                    if (ReachableNodes.find(ConnectedNode) == ReachableNodes.end())
                    {
                        continue;
                    }

                    if (--InDegree[ConnectedNode] == 0)
                    {
                        ReadyQueue.push(ConnectedNode);
                    }
                }
            }

            if (auto It = ImplicitDependents.find(Node); It != ImplicitDependents.end())
            {
                for (CEdGraphNode* Dependent : It->second)
                {
                    if (--InDegree[Dependent] == 0)
                    {
                        ReadyQueue.push(Dependent);
                    }
                }
            }
        }

        if (ProcessedNodeCount != ReachableNodes.size())
        {
            for (auto& Pair : InDegree)
            {
                if (Pair.second > 0)
                {
                    SortedNodes.clear();
                    return Pair.first;
                }
            }
        }

        return nullptr;
    }

    // Reverse-BFS the input edges from RootNode, collecting every node it depends on.
    inline void CollectReachableFromRoot(CEdGraphNode* RootNode, THashSet<CEdGraphNode*>& ReachableNodes)
    {
        TQueue<CEdGraphNode*> ReverseQueue;
        if (ReachableNodes.insert(RootNode).second)
        {
            ReverseQueue.push(RootNode);
        }

        while (!ReverseQueue.empty())
        {
            CEdGraphNode* Node = ReverseQueue.front();
            ReverseQueue.pop();

            for (CEdNodeGraphPin* InputPin : Node->GetInputPins())
            {
                for (CEdNodeGraphPin* ConnectedPin : InputPin->GetConnections())
                {
                    CEdGraphNode* ConnectedNode = ConnectedPin->GetOwningNode();
                    if (ReachableNodes.insert(ConnectedNode).second)
                    {
                        ReverseQueue.push(ConnectedNode);
                    }
                }
            }

            // A named reroute usage has no input pin, so without this its declaration and everything
            // feeding it would be judged unreachable and never compiled.
            if (CEdGraphNode* Implicit = Node->GetImplicitInputNode())
            {
                if (ReachableNodes.insert(Implicit).second)
                {
                    ReverseQueue.push(Implicit);
                }
            }
        }
    }

    // Topo-sorts the subset of Nodes reachable (via input edges) from the first RootPredicate match
    // into SortedNodes (dependency order, roots-last). Returns the first node in a cycle, else nullptr.
    template <typename TRootPredicate>
    CEdGraphNode* TopologicalSortFromRoot(const TVector<TObjectPtr<CEdGraphNode>>& Nodes, TVector<CEdGraphNode*>& SortedNodes, TRootPredicate&& IsRoot)
    {
        CEdGraphNode* RootNode = nullptr;
        for (CEdGraphNode* Node : Nodes)
        {
            if (IsRoot(Node))
            {
                RootNode = Node;
                break;
            }
        }

        if (RootNode == nullptr)
        {
            SortedNodes.clear();
            return nullptr;
        }

        THashSet<CEdGraphNode*> ReachableNodes;
        CollectReachableFromRoot(RootNode, ReachableNodes);
        return TopologicalSortReachable(Nodes, ReachableNodes, SortedNodes);
    }

    // Multi-root variant: seeds the reachability walk from EVERY node matching RootPredicate. A
    // material function graph has one output node per declared output, so the walk must seed from all.
    template <typename TRootPredicate>
    CEdGraphNode* TopologicalSortFromRoots(const TVector<TObjectPtr<CEdGraphNode>>& Nodes, TVector<CEdGraphNode*>& SortedNodes, TRootPredicate&& IsRoot)
    {
        THashSet<CEdGraphNode*> ReachableNodes;
        for (CEdGraphNode* Node : Nodes)
        {
            if (IsRoot(Node))
            {
                CollectReachableFromRoot(Node, ReachableNodes);
            }
        }

        if (ReachableNodes.empty())
        {
            SortedNodes.clear();
            return nullptr;
        }

        return TopologicalSortReachable(Nodes, ReachableNodes, SortedNodes);
    }
}
