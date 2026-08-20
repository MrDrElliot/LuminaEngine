#pragma once

#include <entt/entt.hpp>

#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Core/Object/ObjectMacros.h"
#include "SceneFolderComponent.generated.h"

namespace Lumina
{
    // One outliner folder: a name, an optional parent folder, and the entities filed under it.
    REFLECT()
    struct RUNTIME_API FSceneFolder
    {
        GENERATED_BODY()

        PROPERTY()
        uint32 ID = 0;

        /** Owning folder, or 0 for a folder that sits at the root of the outliner. */
        PROPERTY()
        uint32 ParentID = 0;

        PROPERTY()
        FName Name;

        /** Members, as entity integral ids. Only entities without an entity parent are filed here. */
        PROPERTY(Entity)
        TVector<uint32> Entities;
    };

    // Outliner-only grouping, stored on the world's singleton entity, which is itself hidden from the outliner.
    REFLECT(Component, HideInComponentList, HideInDetails)
    struct RUNTIME_API SSceneFolderComponent
    {
        GENERATED_BODY()

        static constexpr uint32 NoFolder = 0;

        PROPERTY()
        TVector<FSceneFolder> Folders;

        /** Monotonic id source; ids are never reused so a stale reference can't alias a new folder. */
        PROPERTY()
        uint32 NextFolderID = 1;

        NODISCARD FSceneFolder* Find(uint32 FolderID)
        {
            for (FSceneFolder& Folder : Folders)
            {
                if (Folder.ID == FolderID && FolderID != NoFolder)
                {
                    return &Folder;
                }
            }
            return nullptr;
        }

        NODISCARD const FSceneFolder* Find(uint32 FolderID) const
        {
            return const_cast<SSceneFolderComponent*>(this)->Find(FolderID);
        }

        NODISCARD bool IsEmpty() const { return Folders.empty(); }

        uint32 CreateFolder(const FName& Name, uint32 ParentID)
        {
            const uint32 ResolvedParent = Find(ParentID) != nullptr ? ParentID : NoFolder;

            FSceneFolder& Folder = Folders.emplace_back();
            Folder.ID = NextFolderID++;
            Folder.ParentID = ResolvedParent;
            Folder.Name = Name;
            return Folder.ID;
        }

        /** Returns the folder of this name under ParentID, created if there is not one yet. */
        uint32 FindOrCreateFolder(const FName& Name, uint32 ParentID = NoFolder)
        {
            for (const FSceneFolder& Folder : Folders)
            {
                if (Folder.Name == Name && Folder.ParentID == ParentID)
                {
                    return Folder.ID;
                }
            }
            return CreateFolder(Name, ParentID);
        }

        void RenameFolder(uint32 FolderID, const FName& NewName)
        {
            if (FSceneFolder* Folder = Find(FolderID))
            {
                Folder->Name = NewName;
            }
        }

        NODISCARD bool IsDescendantOf(uint32 FolderID, uint32 PossibleAncestor) const
        {
            uint32 Cursor = FolderID;
            for (uint32 Guard = 0; Cursor != NoFolder && Guard < Folders.size() + 1; ++Guard)
            {
                if (Cursor == PossibleAncestor)
                {
                    return true;
                }
                const FSceneFolder* Folder = Find(Cursor);
                Cursor = Folder != nullptr ? Folder->ParentID : NoFolder;
            }
            return false;
        }

        /** Reparents a folder. Rejected when the new parent is the folder itself or one of its descendants. */
        bool SetFolderParent(uint32 FolderID, uint32 NewParentID)
        {
            FSceneFolder* Folder = Find(FolderID);
            if (Folder == nullptr || FolderID == NewParentID)
            {
                return false;
            }
            if (NewParentID != NoFolder && (Find(NewParentID) == nullptr || IsDescendantOf(NewParentID, FolderID)))
            {
                return false;
            }

            Folder->ParentID = NewParentID;
            return true;
        }

        void CollectDescendants(uint32 FolderID, TVector<uint32>& OutFolders) const
        {
            for (const FSceneFolder& Folder : Folders)
            {
                if (Folder.ParentID == FolderID)
                {
                    OutFolders.push_back(Folder.ID);
                    CollectDescendants(Folder.ID, OutFolders);
                }
            }
        }

        /** Deletes one folder; its child folders and members are adopted by its parent. */
        void RemoveFolder(uint32 FolderID)
        {
            const FSceneFolder* Folder = Find(FolderID);
            if (Folder == nullptr)
            {
                return;
            }

            const uint32 NewParent = Folder->ParentID;
            TVector<uint32> Orphans = Folder->Entities;

            for (FSceneFolder& Other : Folders)
            {
                if (Other.ParentID == FolderID)
                {
                    Other.ParentID = NewParent;
                }
            }

            for (auto It = Folders.begin(); It != Folders.end(); ++It)
            {
                if (It->ID == FolderID)
                {
                    Folders.erase(It);
                    break;
                }
            }

            if (FSceneFolder* Parent = Find(NewParent))
            {
                Parent->Entities.insert(Parent->Entities.end(), Orphans.begin(), Orphans.end());
            }
        }

        NODISCARD uint32 FindEntityFolder(entt::entity Entity) const
        {
            const uint32 Handle = static_cast<uint32>(entt::to_integral(Entity));
            for (const FSceneFolder& Folder : Folders)
            {
                for (uint32 Member : Folder.Entities)
                {
                    if (Member == Handle)
                    {
                        return Folder.ID;
                    }
                }
            }
            return NoFolder;
        }

        /** Files an entity under FolderName, creating it if it does not already exist,
         * removing it from whichever folder held it. NoFolder unfiles it.
         */
        void AssignEntity(entt::entity Entity, FName FolderName)
        {
            if (uint32 FolderID = FindOrCreateFolder(FolderName))
            {
                AssignEntity(Entity, FolderID);
            }
        }
        
        /** Files an entity under FolderID, removing it from whichever folder held it. NoFolder unfiles it. */
        void AssignEntity(entt::entity Entity, uint32 FolderID)
        {
            RemoveEntity(Entity);

            if (FSceneFolder* Folder = Find(FolderID))
            {
                Folder->Entities.push_back(static_cast<uint32>(entt::to_integral(Entity)));
            }
        }

        void RemoveEntity(entt::entity Entity)
        {
            const uint32 Handle = static_cast<uint32>(entt::to_integral(Entity));
            for (FSceneFolder& Folder : Folders)
            {
                for (auto It = Folder.Entities.begin(); It != Folder.Entities.end(); ++It)
                {
                    if (*It == Handle)
                    {
                        Folder.Entities.erase(It);
                        return;
                    }
                }
            }
        }
    };
}
