#pragma once

#include "Containers/HashTable.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Versioning/CoreVersion.h"
#include "SaveGame.generated.h"

namespace Lumina::ECS
{
    class FRegistry;
}

namespace Lumina
{
    class CWorld;

    // What a game keeps between sessions. Subclass it in C++ or C#, and every serializable property is saved.
    REFLECT(Scriptable)
    class RUNTIME_API CSaveGame : public CObject
    {
        GENERATED_BODY()

    public:

        // Seconds since 1970 when this was last written to a slot, zero before then.
        PROPERTY(ReadOnly)
        int64 SavedUnixTime = 0;

        // Stores the world's SaveGame state under its level, replacing whatever was stored for that level.
        FUNCTION()
        bool CaptureWorld(CWorld* World);

        // Applies what CaptureWorld stored for this world's level. False when nothing was stored for it.
        FUNCTION()
        bool RestoreWorld(CWorld* World);

        FUNCTION()
        bool HasWorldState(CWorld* World) const;

        FUNCTION()
        void ClearWorldStates();

        // Runs before the object is written, so derived fields can be gathered into saved ones.
        FUNCTION()
        virtual void OnBeforeSave() {}

        // Runs once a load has filled every field.
        FUNCTION()
        virtual void OnAfterLoad() {}

        void Serialize(FArchive& Ar) override;

        bool CaptureRegistry(const FString& Key, ECS::FRegistry& Registry);
        bool RestoreRegistry(const FString& Key, ECS::FRegistry& Registry) const;

    private:

        THashMap<FString, TVector<uint8>> WorldStates;

        // Stored world states are read with the property format of the engine that wrote them.
        int32 LoadedFileVersion = GPackageFileLuminaVersion.FileVersion;
    };
}
