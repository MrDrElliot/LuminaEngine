#pragma once

#include "Containers/Function.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Core/Serialization/ProxyArchive.h"
#include "World/ECS/Entity.h"

namespace Lumina
{
    class CObject;
    class CStruct;

    // Which properties a property block carries.
    enum class ESaveGameProperties : uint8
    {
        // Every property flagged SaveGame.
        SaveGame,

        // SaveGame properties the ordinary serializer skips, the ones flagged NoSerialize as well.
        SaveGameOnly,
    };

    // An asset is saved by GUID, and an object made at runtime is written inline once, however often it is referenced.
    class RUNTIME_API FSaveGameArchive : public FProxyArchive
    {
    public:

        explicit FSaveGameArchive(FArchive& InInnerAr);

        using FProxyArchive::operator<<;

        FArchive& operator<<(CObject*& Value) override;
        FArchive& operator<<(FObjectHandle& Value) override;
        void SerializeEntityId(uint32& PackedEntity) override;

        // Read ids are passed through this, so data saved in one world names the right entities in another.
        TFunction<ECS::FEntity(ECS::FEntity)> TranslateEntity;

    private:

        THashMap<CObject*, uint32> WrittenObjects;
        TVector<CObject*>          ReadObjects;
    };

    namespace SaveGame
    {
        // Name, type and length per property, so a reader skips what it no longer has instead of failing.
        RUNTIME_API void SerializeProperties(FArchive& Ar, const CStruct* Struct, void* Data, ESaveGameProperties Which);

        // Everything an object persists, its own Serialize plus SaveGame fields that never reach a package.
        RUNTIME_API void SerializeObjectState(FArchive& Ar, CObject* Object);

        RUNTIME_API bool HasSaveGameProperties(const CStruct* Struct);
    }
}
