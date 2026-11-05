#pragma once

#include "World/ECS/Registry.h"



#include "Containers/Algorithm.h"
#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Core/Object/ObjectMacros.h"
#include "SceneFolderComponent.generated.h"

namespace Lumina
{
    // One outliner folder: a name, an optional parent folder, and the entities filed under it.
    REFLECT()
    struct RUNTIME_API SSceneFolder
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        uint32 ID = 0;

        /** Owning folder, or 0 for a folder that sits at the root of the outliner. */
        PROPERTY(Hidden)
        uint32 ParentID = 0;

        PROPERTY(Hidden)
        FName Name;

        /** Members, as entity integral ids. Only entities without an entity parent are filed here. */
        PROPERTY(Hidden)
        TVector<ECS::FEntity> Entities;
    };

    // Outliner-only grouping, stored on the world's singleton entity, which is itself hidden from the outliner.
    REFLECT(Component, HideInComponentList, HideInDetails)
    struct RUNTIME_API SSceneFolderComponent
    {
        GENERATED_BODY()

        static constexpr uint32 NoFolder = 0;

        PROPERTY(Hidden)
        TVector<SSceneFolder> Folders;

        /** Monotonic id source; ids are never reused so a stale reference can't alias a new folder. */
        PROPERTY(Hidden)
        uint32 NextFolderID = 1;

        NODISCARD SSceneFolder* Find(uint32 FolderID)
        {
            for (SSceneFolder& Folder : Folders)
            {
                if (Folder.ID == FolderID && FolderID != NoFolder)
                {
                    return &Folder;
                }
            }
            return nullptr;
        }

        NODISCARD const SSceneFolder* Find(uint32 FolderID) const
        {
            return const_cast<SSceneFolderComponent*>(this)->Find(FolderID);
        }

        NODISCARD bool IsEmpty() const { return Folders.IsEmpty(); }

        uint32 CreateFolder(const FName& Name, uint32 ParentID)
        {
            const uint32 ResolvedParent = Find(ParentID) != nullptr ? ParentID : NoFolder;

            SSceneFolder& Folder = Folders.EmplaceGetRef();
            Folder.ID = NextFolderID++;
            Folder.ParentID = ResolvedParent;
            Folder.Name = Name;
            return Folder.ID;
        }

        /** Returns the folder of this name under ParentID, created if there is not one yet. */
        uint32 FindOrCreateFolder(const FName& Name, uint32 ParentID = NoFolder)
        {
            for (const SSceneFolder& Folder : Folders)
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
            if (SSceneFolder* Folder = Find(FolderID))
            {
                Folder->Name = NewName;
            }
        }

        NODISCARD bool IsDescendantOf(uint32 FolderID, uint32 PossibleAncestor) const
        {
            uint32 Cursor = FolderID;
            for (int32 Guard = 0; Cursor != NoFolder && Guard < Folders.Num() + 1; ++Guard)
            {
                if (Cursor == PossibleAncestor)
                {
                    return true;
                }
                const SSceneFolder* Folder = Find(Cursor);
                Cursor = Folder != nullptr ? Folder->ParentID : NoFolder;
            }
            return false;
        }

        /** Reparents a folder. Rejected when the new parent is the folder itself or one of its descendants. */
        bool SetFolderParent(uint32 FolderID, uint32 NewParentID)
        {
            SSceneFolder* Folder = Find(FolderID);
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
            for (const SSceneFolder& Folder : Folders)
            {
                if (Folder.ParentID == FolderID)
                {
                    OutFolders.Add(Folder.ID);
                    CollectDescendants(Folder.ID, OutFolders);
                }
            }
        }

        /** Deletes one folder; its child folders and members are adopted by its parent. */
        void RemoveFolder(uint32 FolderID)
        {
            const SSceneFolder* Folder = Find(FolderID);
            if (Folder == nullptr)
            {
                return;
            }

            const uint32 NewParent = Folder->ParentID;
            TVector<ECS::FEntity> Orphans = Folder->Entities;

            for (SSceneFolder& Other : Folders)
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
                    Folders.RemoveAt(static_cast<int32>(It - Folders.begin()));
                    break;
                }
            }

            if (SSceneFolder* Parent = Find(NewParent))
            {
                Parent->Entities.Append(Orphans);
            }
        }

        NODISCARD uint32 FindEntityFolder(ECS::FEntity Entity) const
        {
            for (const SSceneFolder& Folder : Folders)
            {
                for (ECS::FEntity Member : Folder.Entities)
                {
                    if (Member == Entity)
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
        void AssignEntity(ECS::FEntity Entity, FName FolderName)
        {
            if (uint32 FolderID = FindOrCreateFolder(FolderName))
            {
                AssignEntity(Entity, FolderID);
            }
        }
        
        /** Files an entity under FolderID, removing it from whichever folder held it. NoFolder unfiles it. */
        void AssignEntity(ECS::FEntity Entity, uint32 FolderID)
        {
            RemoveEntity(Entity);

            if (SSceneFolder* Folder = Find(FolderID))
            {
                Folder->Entities.Add(Entity);
            }
        }

        // One pass over every folder for a whole batch, where RemoveEntity searches them per entity.
        template <typename TPredicate>
        void RemoveEntitiesIf(TPredicate&& ShouldRemove)
        {
            for (SSceneFolder& Folder : Folders)
            {
                Folder.Entities.RemoveAll(ShouldRemove);
            }
        }

        void RemoveEntity(ECS::FEntity Entity)
        {
            for (SSceneFolder& Folder : Folders)
            {
                for (auto It = Folder.Entities.begin(); It != Folder.Entities.end(); ++It)
                {
                    if (*It == Entity)
                    {
                        Folder.Entities.RemoveAt(static_cast<int32>(It - Folder.Entities.begin()));
                        return;
                    }
                }
            }
        }
    };
}
