#pragma once

#include "Containers/Algorithm.h"
#include "Containers/Queue.h"
#include "EdGraphNode.h"
#include "EdNodeGraphPin.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"

namespace Lumina::GraphAlgorithms
{
    // Kahn's algorithm over a reachable set, roots last. Returns the first node in a cycle, or nullptr.
    inline CEdGraphNode* TopologicalSortReachable(const TVector<TStrongObjectPtr<CEdGraphNode>>& Nodes, const THashSet<CEdGraphNode*>& ReachableNodes, TVector<CEdGraphNode*>& SortedNodes)
    {
        THashMap<CEdGraphNode*, uint32> InDegree;
        for (const auto& NodeRef : Nodes)
        {
            CEdGraphNode* Node = NodeRef.Get();
            if (ReachableNodes.Contains(Node))
            {
                InDegree[Node] = 0;
            }
        }

        // Wireless edges carry no pin connection, and counting them like real ones reports a usage above its declaration as a cycle.
        THashMap<CEdGraphNode*, TVector<CEdGraphNode*>> ImplicitDependents;
        for (const auto& NodeRef : Nodes)
        {
            CEdGraphNode* Node = NodeRef.Get();
            if (!ReachableNodes.Contains(Node))
            {
                continue;
            }

            CEdGraphNode* Implicit = Node->GetImplicitInputNode();
            if (Implicit != nullptr && Implicit != Node && ReachableNodes.Contains(Implicit))
            {
                ImplicitDependents[Implicit].Add(Node);
                InDegree[Node]++;
            }
        }

        for (const auto& NodeRef : Nodes)
        {
            CEdGraphNode* Node = NodeRef.Get();
            if (!ReachableNodes.Contains(Node))
            {
                continue;
            }

            for (const auto& OutputPinRef : Node->GetOutputPins())
            {
                CEdNodeGraphPin* OutputPin = OutputPinRef.Get();
                for (CEdNodeGraphPin* ConnectedPin : OutputPin->GetConnections())
                {
                    CEdGraphNode* ConnectedNode = ConnectedPin->GetOwningNode();
                    if (ReachableNodes.Contains(ConnectedNode))
                    {
                        InDegree[ConnectedNode]++;
                    }
                }
            }
        }

        TQueue<CEdGraphNode*> ReadyQueue;
        for (auto& [Node, Degree] : InDegree)
        {
            if (Degree == 0)
            {
                ReadyQueue.Enqueue(Node);
            }
        }

        uint32 ProcessedNodeCount = 0;
        while (!ReadyQueue.IsEmpty())
        {
            CEdGraphNode* Node = ReadyQueue.Peek();
            ReadyQueue.Pop();
            SortedNodes.Add(Node);
            ProcessedNodeCount++;

            for (const auto& OutputPinRef : Node->GetOutputPins())
            {
                CEdNodeGraphPin* OutputPin = OutputPinRef.Get();
                for (CEdNodeGraphPin* ConnectedPin : OutputPin->GetConnections())
                {
                    CEdGraphNode* ConnectedNode = ConnectedPin->GetOwningNode();
                    if (!ReachableNodes.Contains(ConnectedNode))
                    {
                        continue;
                    }

                    if (--InDegree[ConnectedNode] == 0)
                    {
                        ReadyQueue.Enqueue(ConnectedNode);
                    }
                }
            }

            if (auto It = ImplicitDependents.Find(Node); It != nullptr)
            {
                for (CEdGraphNode* Dependent : (*It))
                {
                    if (--InDegree[Dependent] == 0)
                    {
                        ReadyQueue.Enqueue(Dependent);
                    }
                }
            }
        }

        if (ProcessedNodeCount != static_cast<uint32>(ReachableNodes.Num()))
        {
            const auto Cyclic = Algo::FindIf(InDegree, [](const auto& Pair) { return Pair.Value > 0; });
            if (Cyclic != InDegree.end())
            {
                SortedNodes.Reset();
                return Cyclic->Key;
            }
        }

        return nullptr;
    }

    // Reverse-BFS the input edges from RootNode, collecting every node it depends on.
    inline void CollectReachableFromRoot(CEdGraphNode* RootNode, THashSet<CEdGraphNode*>& ReachableNodes)
    {
        TQueue<CEdGraphNode*> ReverseQueue;
        if (ReachableNodes.TryAdd(RootNode))
        {
            ReverseQueue.Enqueue(RootNode);
        }

        while (!ReverseQueue.IsEmpty())
        {
            CEdGraphNode* Node = ReverseQueue.Peek();
            ReverseQueue.Pop();

            const auto Descend = [&ReachableNodes, &ReverseQueue](CEdNodeGraphPin* Pin)
            {
                for (CEdNodeGraphPin* ConnectedPin : Pin->GetConnections())
                {
                    CEdGraphNode* ConnectedNode = ConnectedPin->GetOwningNode();
                    if (ReachableNodes.TryAdd(ConnectedNode))
                    {
                        ReverseQueue.Enqueue(ConnectedNode);
                    }
                }
            };

            // Resolving a passthrough through its single source is what makes a dead branch unreachable.
            if (Node->IsRerouteNode())
            {
                if (CEdNodeGraphPin* Source = Node->GetRerouteSourcePin())
                {
                    Descend(Source);
                }
            }
            else
            {
                for (const auto& InputPinRef : Node->GetInputPins())
                {
                    CEdNodeGraphPin* InputPin = InputPinRef.Get();
                    Descend(InputPin);
                }
            }

            // A named reroute usage has no input pin, so its declaration would otherwise be judged unreachable.
            if (CEdGraphNode* Implicit = Node->GetImplicitInputNode())
            {
                if (ReachableNodes.TryAdd(Implicit))
                {
                    ReverseQueue.Enqueue(Implicit);
                }
            }
        }
    }

    // Sorts the nodes reachable from the first RootPredicate match, returning the first node in a cycle or nullptr.
    template <typename TRootPredicate>
    CEdGraphNode* TopologicalSortFromRoot(const TVector<TStrongObjectPtr<CEdGraphNode>>& Nodes, TVector<CEdGraphNode*>& SortedNodes, TRootPredicate&& IsRoot)
    {
        const auto RootItr = Algo::FindIf(Nodes,
            [&IsRoot](const TStrongObjectPtr<CEdGraphNode>& Node) { return IsRoot(Node.Get()); });

        CEdGraphNode* RootNode = RootItr != Nodes.end() ? RootItr->Get() : nullptr;

        if (RootNode == nullptr)
        {
            SortedNodes.Reset();
            return nullptr;
        }

        THashSet<CEdGraphNode*> ReachableNodes;
        CollectReachableFromRoot(RootNode, ReachableNodes);
        return TopologicalSortReachable(Nodes, ReachableNodes, SortedNodes);
    }

    // Seeds the walk from every RootPredicate match, since a function graph has one output node per declared output.
    template <typename TRootPredicate>
    CEdGraphNode* TopologicalSortFromRoots(const TVector<TStrongObjectPtr<CEdGraphNode>>& Nodes, TVector<CEdGraphNode*>& SortedNodes, TRootPredicate&& IsRoot)
    {
        THashSet<CEdGraphNode*> ReachableNodes;
        for (const auto& NodeRef : Nodes)
        {
            CEdGraphNode* Node = NodeRef.Get();
            if (IsRoot(Node))
            {
                CollectReachableFromRoot(Node, ReachableNodes);
            }
        }

        if (ReachableNodes.IsEmpty())
        {
            SortedNodes.Reset();
            return nullptr;
        }

        return TopologicalSortReachable(Nodes, ReachableNodes, SortedNodes);
    }
}
