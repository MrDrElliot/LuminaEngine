#pragma once

#include "Assets/AssetRegistry/AssetData.h"
#include "Containers/Array.h"
#include "Containers/Name.h"
#include "GUID/GUID.h"

namespace Lumina
{
    class FAssetRegistry;
    struct FCookRoot;

    // One node in the cook-time reachability graph. Lives only for the
    // duration of one cook pass; not stored in the asset registry.
    struct FCookNode
    {
        FGuid                       AssetGUID;
        FFixedString                Path;          // resolved virtual path (cached)
        FName                       AssetClass;
        FName                       Chunk;         // assigned during traversal
        EAssetFlags                 EffectiveFlags = EAssetFlags::None;
    };

    // Why an asset failed to enter the graph; surfaces in the cook log to fix dangling roots / missing deps before shipping.
    struct FCookGraphIssue
    {
        FString    Source;          // path or "<root>" of the failing entry
        FString    Detail;
    };

    // Usage: AddRoot() per FEngine::GetCookRoots() entry, Traverse() once, then query IsReachable / GetChunkFor / GetReachableNodes.
    // Walks FAssetData::Dependencies from the registry; never loads packages (discovery/dep extraction is the AssetRegistry's job).
    class FCookGraph
    {
    public:
        explicit FCookGraph(const FAssetRegistry& InRegistry)
            : Registry(&InRegistry) {}

        // Add a seed; resolved paths get a node with the supplied chunk, otherwise an issue is recorded and the root skipped.
        void AddRoot(const FCookRoot& Root);

        // Convenience: bulk add.
        void AddRoots(const TVector<FCookRoot>& Roots);

        // BFS the registry's Dependencies edges by EDependencyType: Hard/Owned inherit referrer's chunk, Soft/Script use the target's own roots, EditorOnly/Generated skipped.
        void Traverse();

        // Post-Traverse queries.
        bool                IsReachable(const FGuid& A) const;
        const FCookNode*    Find(const FGuid& A) const;
        FName               GetChunkFor(const FGuid& A) const;

        // Every reached node in deterministic order (sorted by GUID),
        // so the cooker emits packages in stable order between runs.
        TVector<const FCookNode*> GetReachableNodesSorted() const;

        size_t              NumNodes() const { return Nodes.size(); }
        const TVector<FCookGraphIssue>& GetIssues() const { return Issues; }

    private:
        // Mark target reachable; if not yet present, create node and
        // enqueue for traversal. Returns true if added.
        bool VisitTarget(const FGuid& TargetGUID,
                         const FName& InheritedChunk,
                         EDependencyType EdgeType);

        const FAssetRegistry*           Registry;
        THashMap<FGuid, FCookNode, FGuidHash> Nodes;
        TVector<FGuid>                  Frontier;
        TVector<FCookGraphIssue>        Issues;
    };
}
