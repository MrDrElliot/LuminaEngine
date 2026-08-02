#pragma once
#include "imgui.h"
#include "imgui_internal.h"
#include "Containers/Array.h"
#include "ModuleAPI.h"


namespace Lumina
{
    struct FImDrawListCacheEntry
    {
        ImDrawList* SrcCopy      = nullptr; // Drawlist owned by main ImGui context
        ImDrawList* OurCopy      = nullptr; // Our persistent copy (owns the swapped buffers)
        double      LastUsedTime = 0.0;
    };

    // Render-thread snapshot of one ImGui frame. Owns a persistent pool of
    // ImDrawList copies keyed by source-list pointer; SnapUsingSwap swaps the
    // Cmd/Idx/Vtx buffer storage out of ImGui's live lists into ours, so the
    // game thread can call NewFrame on the next frame while the render thread
    // is still reading from this snapshot.
    //
    // Pattern from ocornut/imgui_club imgui_threaded_rendering.h. After the
    // first frame the swap is allocation-free unless buffers need to grow.
    class FImDrawDataSnapshot
    {
    public:

        FImDrawDataSnapshot() = default;
        ~FImDrawDataSnapshot() { Clear(); }

        FImDrawDataSnapshot(const FImDrawDataSnapshot&)            = delete;
        FImDrawDataSnapshot& operator=(const FImDrawDataSnapshot&) = delete;

        // Swap ImGui's live draw lists into our persistent copies. Source must
        // be the result of ImGui::GetDrawData() on the game thread, immediately
        // after ImGui::Render(). Passing null clears the snapshot.
        void SnapUsingSwap(ImDrawData* Source, double CurrentTime)
        {
            if (Source == nullptr || !Source->Valid)
            {
                DrawData = {};
                bValid = false;
                return;
            }

            ImDrawData* Dst = &DrawData;

            // Shallow-copy every field except CmdLists[] (the owning vector).
            ImVector<ImDrawList*> BackupCmdLists;
            BackupCmdLists.swap(Source->CmdLists);
            
            ImVector<ImDrawList*> KeepDstCmdLists;
            KeepDstCmdLists.swap(Dst->CmdLists);

            *Dst = *Source;

            BackupCmdLists.swap(Source->CmdLists);
            KeepDstCmdLists.swap(Dst->CmdLists);

            Dst->CmdLists.resize(0);   // keeps capacity

            for (ImDrawList* SrcList : Source->CmdLists)
            {
                if (SrcList == nullptr)
                {
                    continue;
                }

                FImDrawListCacheEntry* Entry = GetOrAddEntry(SrcList);
                if (Entry->OurCopy == nullptr)
                {
                    Entry->SrcCopy = SrcList;
                    Entry->OurCopy = IM_NEW(ImDrawList)(SrcList->_Data);
                }
                IM_ASSERT(Entry->SrcCopy == SrcList);

                Entry->SrcCopy->CmdBuffer.swap(Entry->OurCopy->CmdBuffer);
                Entry->SrcCopy->IdxBuffer.swap(Entry->OurCopy->IdxBuffer);
                Entry->SrcCopy->VtxBuffer.swap(Entry->OurCopy->VtxBuffer);

                // Hand the source the bigger capacity so it doesn't realloc next frame.
                Entry->SrcCopy->CmdBuffer.reserve(Entry->OurCopy->CmdBuffer.Capacity);
                Entry->SrcCopy->IdxBuffer.reserve(Entry->OurCopy->IdxBuffer.Capacity);
                Entry->SrcCopy->VtxBuffer.reserve(Entry->OurCopy->VtxBuffer.Capacity);

                Entry->LastUsedTime = CurrentTime;
                Dst->CmdLists.push_back(Entry->OurCopy);
            }

            // ImGui maintains the invariant CmdLists.Size == CmdListsCount and
            // backends iterate by CmdListsCount. *Dst = *Source above copied
            // Source's CmdListsCount while Source->CmdLists was swapped out,
            // so resync to whatever we actually pushed.
            Dst->CmdListsCount = Dst->CmdLists.Size;

            // GC pool entries for lists that haven't been seen for a while
            // (closed windows, destroyed viewports).
            const double GCThreshold = CurrentTime - static_cast<double>(MemoryCompactSeconds);
            for (int n = 0; n < Cache.GetMapSize(); ++n)
            {
                if (FImDrawListCacheEntry* Entry = Cache.TryGetMapData(n))
                {
                    if (Entry->LastUsedTime > GCThreshold)
                    {
                        continue;
                    }
                    IM_DELETE(Entry->OurCopy);
                    Cache.Remove(GetDrawListID(Entry->SrcCopy), Entry);
                }
            }

            bValid = true;
        }

        // Must be called while the owning ImGui context is still alive, the
        // pooled ImDrawLists were allocated through the context's allocator.
        void Clear()
        {
            for (int n = 0; n < Cache.GetMapSize(); ++n)
            {
                if (FImDrawListCacheEntry* Entry = Cache.TryGetMapData(n))
                {
                    IM_DELETE(Entry->OurCopy);
                }
            }
            Cache.Clear();
            DrawData = {};
            bValid = false;
        }

        bool IsValid() const { return bValid; }
        ImDrawData* GetDrawData() { return bValid ? &DrawData : nullptr; }

    private:

        ImGuiID GetDrawListID(ImDrawList* SrcList) const
        {
            return ImHashData(&SrcList, sizeof(SrcList));
        }

        FImDrawListCacheEntry* GetOrAddEntry(ImDrawList* SrcList)
        {
            return Cache.GetOrAddByKey(GetDrawListID(SrcList));
        }

        ImDrawData                       DrawData = {};
        ImPool<FImDrawListCacheEntry>    Cache;
        float                            MemoryCompactSeconds = 20.0f;
        bool                             bValid = false;
    };
}
