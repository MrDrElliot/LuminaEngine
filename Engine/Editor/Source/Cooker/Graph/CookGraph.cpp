#include "CookGraph.h"

#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetRegistry/CookRoot.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"

namespace Lumina
{
    void FCookGraph::AddRoot(const FCookRoot& Root)
    {
        // Accept either bare "/Game/Maps/X" or full "/Game/Maps/X.lasset".
        const FFixedString Resolved = VFS::ResolveToVirtualPath(Root.Asset);
        const FStringView View(Resolved.c_str(), Resolved.size());

        FAssetData* Data = Registry->GetAssetByPath(View);
        if (!Data)
        {
            FCookGraphIssue Issue;
            Issue.Source = "<root>";
            Issue.Detail = FString("Cook root '").append(Root.Asset.c_str(), Root.Asset.size())
                + "' did not resolve to a registered asset (configured as "
                + FString(Resolved.c_str(), Resolved.size()) + ").";
            Issues.emplace_back(Move(Issue));
            return;
        }

        const FName Chunk = Root.Chunk.IsNone() ? FName("Main") : Root.Chunk;
        VisitTarget(Data->AssetGUID, Chunk, EDependencyType::Hard);
    }

    void FCookGraph::AddRoots(const TVector<FCookRoot>& Roots)
    {
        for (const FCookRoot& R : Roots)
        {
            AddRoot(R);
        }
    }

    bool FCookGraph::VisitTarget(const FGuid& TargetGUID,
                                 const FName& InheritedChunk,
                                 EDependencyType EdgeType)
    {
        // Editor-only edges are pruned for cooked output entirely.
        if (EdgeType == EDependencyType::EditorOnly)        return false;
        if (EdgeType == EDependencyType::Generated)         return false;

        auto It = Nodes.find(TargetGUID);
        if (It != Nodes.end())
        {
            // Already in graph. Owned-edge wins: it pulls target into the
            // referrer's chunk regardless of prior assignment.
            if (EdgeType == EDependencyType::Owned)
            {
                It->second.Chunk = InheritedChunk;
            }
            // Soft/Script don't change chunk; first Hard root wins.
            return false;
        }

        FAssetData* Data = Registry->GetAssetByGUID(TargetGUID);
        if (!Data)
        {
            // Reachable GUID with no asset record (engine CDOs / CClass / CStruct): not cookable, silently ignored. Real dangling refs still cook; runtime warns on load.
            return false;
        }

        // Asset-level NeverCook: don't enter the graph + record an issue.
        if (HasFlag(Data->Flags, EAssetFlags::NeverCook))
        {
            FCookGraphIssue Issue;
            Issue.Source = FString(Data->Path.c_str(), Data->Path.size());
            Issue.Detail = "Asset is marked NeverCook but was reached via the dependency graph; check inbound references.";
            Issues.emplace_back(Move(Issue));
            return false;
        }
        // Editor-only assets always stripped.
        if (HasFlag(Data->Flags, EAssetFlags::EditorOnly))
        {
            return false;
        }

        FCookNode Node;
        Node.AssetGUID      = Data->AssetGUID;
        Node.Path           = Data->Path;
        Node.AssetClass     = Data->AssetClass;
        Node.Chunk          = InheritedChunk;
        Node.EffectiveFlags = Data->Flags;

        Nodes.emplace(TargetGUID, Move(Node));
        Frontier.push_back(TargetGUID);
        return true;
    }

    void FCookGraph::Traverse()
    {
        while (!Frontier.empty())
        {
            const FGuid Current = Frontier.back();
            Frontier.pop_back();

            auto NodeIt = Nodes.find(Current);
            if (NodeIt == Nodes.end()) continue; // defensive
            const FName ParentChunk = NodeIt->second.Chunk;

            FAssetData* Data = Registry->GetAssetByGUID(Current);
            if (!Data) continue;

            for (const FAssetDependency& Dep : Data->Dependencies)
            {
                // Owned edges inherit the parent's chunk; everything else
                // also inherits for Phase 1 (chunk routing is Phase 3).
                VisitTarget(Dep.TargetGUID, ParentChunk, Dep.Type);
            }
        }
    }

    bool FCookGraph::IsReachable(const FGuid& A) const
    {
        return Nodes.find(A) != Nodes.end();
    }

    const FCookNode* FCookGraph::Find(const FGuid& A) const
    {
        auto It = Nodes.find(A);
        return It == Nodes.end() ? nullptr : &It->second;
    }

    FName FCookGraph::GetChunkFor(const FGuid& A) const
    {
        auto It = Nodes.find(A);
        return It == Nodes.end() ? FName() : It->second.Chunk;
    }

    TVector<const FCookNode*> FCookGraph::GetReachableNodesSorted() const
    {
        // Sort by GUID for deterministic cook output: identical inputs -> identical PAK order -> identical hash (reproducible builds).
        TVector<const FCookNode*> Out;
        Out.reserve(Nodes.size());
        for (const auto& Pair : Nodes)
        {
            Out.push_back(&Pair.second);
        }
        Algo::Sort(Out.begin(), Out.end(), [](const FCookNode* A, const FCookNode* B)
        {
            return A->AssetGUID < B->AssetGUID;
        });
        return Out;
    }
}
