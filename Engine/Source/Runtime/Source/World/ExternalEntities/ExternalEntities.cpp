#include "RuntimePCH.h"
#include "ExternalEntities.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Core/Serialization/ProxyArchive.h"
#include "FileSystem/FileSystem.h"
#include "Paths/Paths.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/DirtyComponent.h"
#include "World/Entity/Components/EditorComponent.h"
#include "World/Entity/Components/EntityGuidComponent.h"
#include "World/Subsystems/WorldSettings.h"
#include "World/World.h"

namespace Lumina
{
    struct FExternalEntityFile
    {
        ECS::FEntity Self;

        // What each entity handle in the file stands for, in the order the file first named them, its own entity first.
        TVector<FName> Guids;

        ECS::Utils::FParentLinks Links;

        bool bRead = false;
    };

    namespace
    {
        // Unreflected so no serialize touches it, which keeps a registry snapshot restore from emptying it before the next save.
        struct FUnreadExternalEntities
        {
            TVector<FName> Guids;
        };

        // Slot 0 is null and slot 1 the file's own entity, so slot N names Guids[N - 1].
        constexpr uint32 kSelfSlot = 1;

        // No live entity carries the tombstone version, so a slot read back this way cannot alias one before it is resolved.
        ECS::FEntity UnresolvedHandle(uint32 Slot)
        {
            return ECS::FEntity(Slot, ECS::FEntity::TombstoneVersion);
        }

        // Entity handles travel as slots in the file's own table, so the bytes never depend on where entities sat in memory.
        class FEntityFileArchive : public FProxyArchive
        {
        public:

            FEntityFileArchive(FArchive& InInner, ECS::FRegistry& InRegistry, FExternalEntityFile& InFile)
                : FProxyArchive(InInner)
                , Registry(InRegistry)
                , File(InFile)
            {
                // A save serializes once to find its exports and again to write, so each pass starts its own table.
                if (IsWriting())
                {
                    File.Guids.Reset();
                    SlotOf(File.Self);
                }
            }

            using FProxyArchive::operator<<;

            // FProxyArchive leaves these to the base, which would keep the entity's assets out of its package's tables.
            FArchive& operator<<(CObject*& Value) override { InnerArchive << Value; return *this; }
            void RegisterSoftAssetReference(const FGuid& AssetGUID) override { InnerArchive.RegisterSoftAssetReference(AssetGUID); }
            FStringView SetSoftReferenceBundles(FStringView Bundles) override { return InnerArchive.SetSoftReferenceBundles(Bundles); }
            bool SupportsBulkData() const override { return InnerArchive.SupportsBulkData(); }
            bool WriteBulkData(FBulkDataRef& OutRef, const void* Data, int64 Size) override { return InnerArchive.WriteBulkData(OutRef, Data, Size); }
            void FlagUnresolvedBulkData() override { InnerArchive.FlagUnresolvedBulkData(); }
            bool DescribeCutBytes(FPortableBytes& Cut) const override { return InnerArchive.DescribeCutBytes(Cut); }

            void SerializeEntityId(uint32& PackedEntity) override
            {
                uint32 Slot = IsWriting() ? SlotOf(ECS::FEntity::FromPacked(PackedEntity)) : 0;
                InnerArchive << Slot;
                if (IsReading())
                {
                    PackedEntity = (Slot == 0 ? ECS::NullEntity : Slot == kSelfSlot ? File.Self : UnresolvedHandle(Slot)).GetPacked();
                }
            }

        private:

            uint32 SlotOf(ECS::FEntity Entity)
            {
                if (!Registry.IsValid(Entity))
                {
                    return 0;
                }
                if (const uint32* Known = Slots.Find(Entity))
                {
                    return *Known;
                }
                File.Guids.Add(ExternalEntities::GuidOf(Registry, Entity));
                return Slots[Entity] = (uint32)File.Guids.Num();
            }

            ECS::FRegistry&                     Registry;
            FExternalEntityFile&                File;
            THashMap<ECS::FEntity, uint32>      Slots;
        };

        // The settings entity is the world's own, so it stays in the world package.
        bool IsWorldLevel(const ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            return Registry.HasAny<SDefaultWorldSettings>(Entity);
        }

        // Bucketed by the id's first two characters, which are random, so no folder grows past a few hundred files.
        FFixedString PackagePath(FStringView WorldPath, const FName& Guid)
        {
            const FStringView Id(*Guid);
            FFixedString Path = Paths::Combine(ExternalEntities::EntitiesDirectory(WorldPath), Id.Mid(0, 2), Id);
            CPackage::AddPackageExt(Path);
            return Path;
        }

        // Entity packages never stay resident, so every save and load starts from what is on disk.
        void UnloadResident(FStringView Path)
        {
            if (CPackage* Resident = CPackage::FindPackageByPath(Path))
            {
                Resident->Unload();
            }
        }

        SDefaultWorldSettings* FindWorldSettings(ECS::FRegistry& Registry)
        {
            auto View = Registry.View<SDefaultWorldSettings>();
            return View.IsEmpty() ? nullptr : &Registry.Get<SDefaultWorldSettings>(*View.begin());
        }

        CExternalEntity* NewExport(CPackage* Package, const FName& Guid, ECS::FRegistry& Registry, FExternalEntityFile& File)
        {
            CExternalEntity* Export = NewObject<CExternalEntity>(Package, Guid, FGuid::NewDeterministic(*Guid));
            Export->Registry = &Registry;
            Export->File = &File;
            return Export;
        }

        bool SavePackage(FStringView WorldPath, const FName& Guid, ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            const FFixedString Path = PackagePath(WorldPath, Guid);
            UnloadResident(Path);
            CPackage* Package = CPackage::CreatePackage(Path);

            FExternalEntityFile File;
            File.Self = Entity;
            CExternalEntity* Export = NewExport(Package, Guid, Registry, File);

            const bool bSaved = CPackage::SavePackage(Package, Path);
            if (bSaved)
            {
                FAssetRegistry::Get().AssetSaved(Export);
            }
            else
            {
                LOG_ERROR("World '{}': entity {} could not be saved to '{}'.", WorldPath, Guid, Path);
            }
            Package->Unload();
            return bSaved;
        }

        // Writes the file of every entity a save owes one, keeping only the guids whose write failed dirty, and returns every entity's guid.
        TVector<FName> SaveEntityPackages(FStringView WorldPath, ECS::FRegistry& Registry, FExternalEntityChanges& Changes)
        {
            FScopedAssetRegistryBatch RegistryBatch;
            TVector<FName> Guids;
            TVector<FName> Failed;
            Registry.ForEachEntityExcept<FEditorComponent>([&](ECS::FEntity Entity)
            {
                const FName Guid = ExternalEntities::GuidOf(Registry, Entity);
                if (Guid.IsNone() || IsWorldLevel(Registry, Entity))
                {
                    return;
                }
                Guids.Add(Guid);
                const bool bOwed = !Changes.bSavingChanges || Changes.Dirty.Contains(Guid) || !VFS::Exists(PackagePath(WorldPath, Guid));
                if (bOwed && !SavePackage(WorldPath, Guid, Registry, Entity))
                {
                    Failed.Add(Guid);
                }
            });
            Changes.Dirty.Reset();
            Changes.Dirty.Append(Failed);
            return Guids;
        }

        bool LoadPackage(FStringView Path, const FName& Guid, ECS::FRegistry& Registry, FExternalEntityFile& File)
        {
            UnloadResident(Path);
            CPackage* Package = VFS::Exists(Path) ? CPackage::LoadPackage(Path) : nullptr;
            if (Package == nullptr)
            {
                return false;
            }

            // Made before the read, since the shell the package would make has no registry to read into.
            CExternalEntity* Export = NewExport(Package, Guid, Registry, File);
            Export->SetFlag(OF_NeedsLoad);
            const bool bRead = Package->LoadObject(Export->GetGUID()) == Export && File.bRead;

            Package->Unload();
            return bRead;
        }

        void DeletePackage(FStringView Path)
        {
            // A file too corrupt to name its export cannot be unregistered, only removed.
            if (!CPackage::DestroyPackage(Path))
            {
                VFS::Remove(Path);
            }
        }

        void DeleteStalePackages(FStringView WorldPath, TSpan<const FName> Manifest)
        {
            const FFixedString Directory = ExternalEntities::EntitiesDirectory(WorldPath);
            if (!VFS::IsDirectory(Directory))
            {
                return;
            }

            THashSet<FName> Live;
            for (const FName& Guid : Manifest)
            {
                Live.Add(Guid);
            }
            TVector<FString> Stale;
            VFS::RecursiveDirectoryIterator(Directory, [&](VFS::FFileInfo& Info)
            {
                if (!Info.IsDirectory() && !Live.Contains(FName(VFS::FileName(Info.VirtualPath, true))))
                {
                    Stale.Add(Info.VirtualPath);
                }
            });

            FScopedAssetRegistryBatch RegistryBatch;
            for (const FString& Path : Stale)
            {
                DeletePackage(Path);
            }
        }

        // Every file is in by now, so each slot resolves through the id it stands for, and one whose entity never loaded is cleared.
        void ResolveEntityReferences(ECS::FRegistry& Registry, TSpan<FExternalEntityFile> Files)
        {
            THashMap<FName, ECS::FEntity> ByGuid;
            Registry.View<SEntityGuidComponent>().ForEach([&](ECS::FEntity Entity, const SEntityGuidComponent& Guid)
            {
                ByGuid[Guid.Guid] = Entity;
            });

            ECS::Utils::FParentLinks Links;
            for (FExternalEntityFile& File : Files)
            {
                THashMap<ECS::FEntity, ECS::FEntity> Map;
                Map[File.Self] = File.Self;
                for (int32 Index = 1; Index < File.Guids.Num(); ++Index)
                {
                    if (const ECS::FEntity* Target = ByGuid.Find(File.Guids[Index]))
                    {
                        Map[UnresolvedHandle(Index + 1)] = *Target;
                    }
                }

                ECS::Utils::RemapEntityReferences(Registry, File.Self, Map, /*bClearUnmapped*/ true);
                for (const ECS::Utils::FParentLinks::FLink& Link : File.Links.Links)
                {
                    if (const ECS::FEntity* Parent = Map.Find(Link.Parent))
                    {
                        Links.Links.Emplace(Link.Child, *Parent, Link.SiblingIndex);
                    }
                }
            }
            Links.Apply(Registry);
        }
    }

    void CExternalEntity::Serialize(FArchive& Ar)
    {
        CObject::Serialize(Ar);
        if (Registry == nullptr || File == nullptr)
        {
            LOG_ERROR("Entity package '{}' is only read and written by the world it belongs to.", GetName());
            return;
        }

        FEntityFileArchive EntityAr(Ar, *Registry, *File);
        const bool bEntityRead = ECS::Utils::SerializeEntity(EntityAr, *Registry, File->Self, &File->Links);
        Ar << File->Guids;
        File->bRead = bEntityRead && !Ar.HasError();
    }

    namespace ExternalEntities
    {
        FFixedString OwningWorldPath(FStringView Path)
        {
            const FStringView WorldDirectory = VFS::Parent(VFS::Parent(Path, true), true);
            const FStringView EntitiesRoot = VFS::Parent(WorldDirectory, true);
            if (VFS::FileName(EntitiesRoot) != kFolderName)
            {
                return {};
            }
            FFixedString WorldPath = Paths::Combine(VFS::Parent(EntitiesRoot, true), VFS::FileName(WorldDirectory));
            CPackage::AddPackageExt(WorldPath);
            return WorldPath;
        }

        bool IsEnabled(ECS::FRegistry& Registry)
        {
            const SDefaultWorldSettings* Settings = FindWorldSettings(Registry);
            return Settings != nullptr && Settings->bExternalEntities;
        }

        void Write(FArchive& Ar, CPackage* WorldPackage, ECS::FRegistry& Registry, FExternalEntityChanges& Changes)
        {
            // Only a package save has somewhere beside the world to put files, so a duplicate or a cook keeps every entity inline.
            bool bSplit = Ar.SupportsBulkData() && !Ar.IsCooking();
            ECS::Utils::SerializeRegistry(Ar, Registry, bSplit ? &IsWorldLevel : nullptr);
            Ar << bSplit;
            if (!bSplit)
            {
                return;
            }

            const FFixedString WorldPath = WorldPackage->GetPackagePath();
            TVector<FName> Manifest = SaveEntityPackages(WorldPath, Registry, Changes);

            // An unreadable file stays listed and untouched for the next load to retry, while one that is gone drops out.
            Registry.View<FUnreadExternalEntities>().ForEach([&](ECS::FEntity, const FUnreadExternalEntities& Unread)
            {
                for (const FName& Guid : Unread.Guids)
                {
                    if (VFS::Exists(PackagePath(WorldPath, Guid)) && !Algo::Contains(Manifest, Guid))
                    {
                        Manifest.Add(Guid);
                    }
                }
            });
            Algo::Sort(Manifest, [](const FName& A, const FName& B) { return FStringView(*A) < FStringView(*B); });
            Ar << Manifest;

            DeleteStalePackages(WorldPath, Manifest);
            Changes.Deleted.Reset();
        }

        void Read(FArchive& Ar, CPackage* WorldPackage, ECS::FRegistry& Registry)
        {
            bool bSplit = false;
            Ar << bSplit;
            if (!bSplit)
            {
                return;
            }

            TVector<FName> Manifest;
            Ar << Manifest;
            const FFixedString WorldPath = WorldPackage->GetPackagePath();
            TVector<FName> Unread;

            // Reserved, since each export holds a pointer into this until its read is done.
            TVector<FExternalEntityFile> Files;
            Files.Reserve(Manifest.Num());
            for (const FName& Guid : Manifest)
            {
                const FFixedString Path = PackagePath(WorldPath, Guid);
                FExternalEntityFile& File = Files.EmplaceGetRef();
                File.Self = Registry.Create();
                if (!LoadPackage(Path, Guid, Registry, File))
                {
                    LOG_WARN("World '{}': entity {} is missing or unreadable at '{}', so the world loads without it.", WorldPath, Guid, Path);
                    Registry.Destroy(File.Self);
                    Files.Pop();
                    Unread.Add(Guid);
                    continue;
                }
                Registry.EmplaceOrReplace<FNeedsTransformUpdate>(File.Self);
            }

            ResolveEntityReferences(Registry, Files);
            Registry.Emplace<FUnreadExternalEntities>(*Registry.View<SDefaultWorldSettings>().begin()).Guids = Move(Unread);
        }

        FName GuidOf(const ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            const SEntityGuidComponent* Guid = Registry.TryGet<SEntityGuidComponent>(Entity);
            return Guid != nullptr ? Guid->Guid : FName();
        }

        void MarkChanged(CWorld* World, TSpan<const ECS::FEntity> Entities, TSpan<const FName> Guids)
        {
            CPackage* Package = World != nullptr ? World->GetPackage() : nullptr;
            if (Package == nullptr)
            {
                return;
            }

            ECS::FRegistry& Registry = World->GetEntityRegistry();
            if (!IsEnabled(Registry))
            {
                Package->MarkDirty();
                return;
            }

            FExternalEntityChanges& Changes = World->GetExternalEntityChanges();
            for (int32 Index = 0; Index < Entities.Num(); ++Index)
            {
                const ECS::FEntity Entity = Entities[Index];
                if (!Registry.IsValid(Entity))
                {
                    if (Index < Guids.Num() && !Guids[Index].IsNone())
                    {
                        Changes.Dirty.Remove(Guids[Index]);
                        Changes.Deleted.Add(Guids[Index]);
                    }
                    continue;
                }

                if (Registry.HasAny<FEditorComponent, FTransientEntityTag>(Entity))
                {
                    continue;
                }

                // An entity with no id yet gets one, and its file, from the next world package save.
                const FName Guid = GuidOf(Registry, Entity);
                if (Guid.IsNone() || IsWorldLevel(Registry, Entity))
                {
                    Package->MarkDirty();
                    continue;
                }
                Changes.Deleted.Remove(Guid);
                Changes.Dirty.Add(Guid);
            }
        }

        bool SaveChanges(CWorld* World)
        {
            CPackage* Package = World->GetPackage();
            const FFixedString WorldPath = Package->GetPackagePath();
            ECS::FRegistry& Registry = World->GetEntityRegistry();
            FExternalEntityChanges& Changes = World->GetExternalEntityChanges();

            // An added or removed entity changes the manifest, which only the world package holds.
            const bool bManifestChanged = !Changes.Deleted.IsEmpty() || Algo::AnyOf(Changes.Dirty, [&](const FName& Guid)
            {
                return !VFS::Exists(PackagePath(WorldPath, Guid));
            });
            Changes.bSavingChanges = true;
            bool bSaved = true;
            if (!IsEnabled(Registry) || Package->IsDirty() || bManifestChanged)
            {
                bSaved = CPackage::SavePackage(Package, WorldPath);
            }
            else
            {
                SaveEntityPackages(WorldPath, Registry, Changes);
            }
            Changes.bSavingChanges = false;

            // A world folded back into its package holds every entity inline, so the files it no longer reads go.
            if (bSaved && !IsEnabled(Registry))
            {
                for (const FName& Guid : Changes.Deleted)
                {
                    if (const FFixedString Path = PackagePath(WorldPath, Guid); VFS::Exists(Path))
                    {
                        DeletePackage(Path);
                    }
                }
                Changes.Deleted.Reset();
            }

            // An entity file that failed to write stays dirty, so the next save retries it.
            return bSaved && Changes.Dirty.IsEmpty();
        }

        FFixedString EntitiesDirectory(FStringView WorldPath)
        {
            return Paths::Combine(VFS::Parent(WorldPath, true), kFolderName, VFS::FileName(WorldPath, true));
        }

        bool DeleteEntityFiles(FStringView WorldPath)
        {
            DeleteStalePackages(WorldPath, {});
            const FFixedString Directory = EntitiesDirectory(WorldPath);
            return !VFS::Exists(Directory) || VFS::RemoveAll(Directory);
        }
    }
}
