#pragma once

#include "Core/Object/Object.h"
#include "World/ECS/Registry.h"
#include "ExternalEntities.generated.h"

namespace Lumina
{
    class CPackage;
    class CWorld;
    struct FExternalEntityFile;

    // Entities edited since the world was last saved, by guid, so a save rewrites only their files.
    struct FExternalEntityChanges
    {
        THashSet<FName> Dirty;
        THashSet<FName> Deleted;

        // Set while SaveChanges runs, so it writes only the dirty or missing entity files.
        bool bSavingChanges = false;

        NODISCARD bool IsEmpty() const { return Dirty.IsEmpty() && Deleted.IsEmpty(); }
    };

    // The one export of an entity package, which reads and writes only through the registry and file its world hands it.
    REFLECT()
    class RUNTIME_API CExternalEntity : public CObject
    {
        GENERATED_BODY()
    public:

        void Serialize(FArchive& Ar) override;

        ECS::FRegistry*         Registry = nullptr;
        FExternalEntityFile*    File = nullptr;
    };

    namespace ExternalEntities
    {
        // The folder beside a world that holds its entity files.
        inline constexpr const char* kFolderName = "__ExternalEntities__";

        // The world package that owns the entity file at Path, or empty when Path is not an entity file.
        NODISCARD RUNTIME_API FFixedString OwningWorldPath(FStringView Path);

        // Writes Registry, and when Ar is a package save keeps only world-level entities inline and gives each other entity its own package beside the world's.
        RUNTIME_API void Write(FArchive& Ar, CPackage* WorldPackage, ECS::FRegistry& Registry, FExternalEntityChanges& Changes);

        // Whether the world whose entities Registry holds keeps them in their own packages.
        NODISCARD RUNTIME_API bool IsEnabled(ECS::FRegistry& Registry);

        // Reads what Write left after the world-level entities, loading each entity package it lists into Registry.
        RUNTIME_API void Read(FArchive& Ar, CPackage* WorldPackage, ECS::FRegistry& Registry);

        // The persistent id Entity is saved under, or none when it has not been given one.
        NODISCARD RUNTIME_API FName GuidOf(const ECS::FRegistry& Registry, ECS::FEntity Entity);

        // Records an edit to Entities so a save rewrites their files, or deletes the file of one that is gone, which Guids names in the same order.
        RUNTIME_API void MarkChanged(CWorld* World, TSpan<const ECS::FEntity> Entities, TSpan<const FName> Guids = {});

        // Saves World, writing only the changed entity files and the world package only when it or the entity list changed, false if any write failed.
        RUNTIME_API bool SaveChanges(CWorld* World);

        // The folder beside the world at WorldPath that holds its entity files.
        NODISCARD RUNTIME_API FFixedString EntitiesDirectory(FStringView WorldPath);

        // Deletes the entity files of the world at WorldPath and their folder, false if any of it remains.
        RUNTIME_API bool DeleteEntityFiles(FStringView WorldPath);
    }
}
