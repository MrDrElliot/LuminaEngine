#pragma once

#include "Containers/Array.h"
#include "Core/Object/Class.h"

namespace Lumina
{
    /**
     * Which node classes belong in which graph, discovered from reflection rather than declared.
     *
     * Every CEdGraphNode subclass is asked (through its CDO) which graph class it belongs to; see
     * CEdGraphNode::GetSupportedGraphClass. Nothing has to be listed anywhere, which is the point: a
     * game or plugin module can declare a material node and it shows up in the palette on the next
     * graph open, with no engine-side edit and no way to forget the registration line.
     *
     * Classes carrying the "NotPlaceable" REFLECT specifier are skipped. Reflection here has no notion
     * of an abstract class -- there are no class flags at all -- so a family base (CMaterialExpression)
     * and a node the graph creates for itself (CMaterialOutputNode) both have to say so explicitly.
     * Metadata rather than a virtual because a virtual would be INHERITED: marking the base would take
     * every leaf with it. Metadata is per-class and stops there.
     *
     * Results are cached per graph class, so the answer must not depend on a graph instance. The cache
     * is dropped when a module loads, since a plugin can bring new node classes at any loading phase --
     * including phases that run after the editor UI is already up.
     */
    class FGraphNodeRegistry
    {
    public:

        static FGraphNodeRegistry& Get();

        /** Node classes offered in a graph of GraphClass. Built on first ask, then cached. */
        const THashSet<CClass*>& GetNodesForGraphClass(CClass* GraphClass);

        /** Drops every cached answer; the next ask rescans. */
        void Invalidate() { Cache.clear(); ++Generation; }

        /**
         * Bumped by every Invalidate. A graph that already filled its palette compares this against the
         * value it built at and refills when they differ, so a plugin loaded while an editor is open
         * still shows up rather than waiting for the tool to be closed and reopened.
         */
        uint32 GetGeneration() const { return Generation; }

    private:

        FGraphNodeRegistry();

        THashSet<CClass*> BuildForGraphClass(CClass* GraphClass) const;

        THashMap<CClass*, THashSet<CClass*>> Cache;
        uint32                               Generation = 0;
    };
}
