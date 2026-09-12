#include "RuntimePCH.h"

#include "DotNetHost.h"
#include "World/Entity/Components/Component.h"
#include "World/Entity/Systems/EntitySystem.h"

// The native side of the C# entity-system bridge. It lives here rather than in the host because an entity
// system's exports are this subsystem's business: the host owns starting and stopping the runtime, not the
// list of things the runtime can be asked to do.
namespace Lumina::DotNet
{
    namespace
    {
        const TManagedExport<void  (*)(void*, void*)>                 EnumerateEntitySystems{ "EnumerateEntitySystems" };
        const TManagedExport<void* (*)(const char*, int32, uint64)>   CreateEntitySystem{ "CreateEntitySystem" };
        const TManagedExport<void  (*)(void*)>                        DestroyEntitySystem{ "DestroyEntitySystem" };
        const TManagedExport<void  (*)(void*, void*)>                 StartupEntitySystem{ "StartupEntitySystem" };
        const TManagedExport<void  (*)(void*, void*)>                 TickEntitySystem{ "TickEntitySystem" };

        // The read and write tokens carry the type hashes used to build the access set.
        void LmCollectAccessIds(const void* const* Tokens, int Count, TVector<uint32>& Out)
        {
            if (Tokens == nullptr || Count <= 0)
            {
                return;
            }
            Out.reserve(static_cast<size_t>(Count));
            for (int i = 0; i < Count; ++i)
            {
                if (const FComponentOps* Ops = static_cast<const FComponentOps*>(Tokens[i]))
                {
                    Out.push_back(static_cast<uint32>(Ops->TypeId));
                }
            }
        }

        void LmSystemDescSink(void* Ctx, const char* Name, int Len, int Stage, int Priority,
            const void* const* WriteTokens, int NWrite, const void* const* ReadTokens, int NRead)
        {
            auto* Out = static_cast<TVector<FManagedSystemDesc>*>(Ctx);
            if (Out != nullptr && Name != nullptr && Len > 0)
            {
                FManagedSystemDesc Desc;
                Desc.TypeName.assign(Name, static_cast<size_t>(Len));
                Desc.Stage = (Stage >= 0 && Stage < (int)EUpdateStage::Max) ? (EUpdateStage)Stage : EUpdateStage::PrePhysics;
                Desc.Priority = Priority;
                LmCollectAccessIds(WriteTokens, NWrite, Desc.Writes);
                LmCollectAccessIds(ReadTokens, NRead, Desc.Reads);
                Out->push_back(std::move(Desc));
            }
        }
    }

    void GatherManagedSystemDescs(TVector<FManagedSystemDesc>& Out)
    {
        Out.clear();
        if (IsInitialized() && EnumerateEntitySystems)
        {
            EnumerateEntitySystems.Get()(reinterpret_cast<void*>(&LmSystemDescSink), &Out);
        }
    }

    void* CreateManagedSystem(FStringView TypeName, uint64 World)
    {
        if (!IsInitialized() || !CreateEntitySystem)
        {
            return nullptr;
        }
        return CreateEntitySystem.Get()(TypeName.data(), (int32)TypeName.size(), World);
    }

    void DestroyManagedSystem(void* Handle)
    {
        if (IsInitialized() && DestroyEntitySystem && Handle)
        {
            DestroyEntitySystem.Get()(Handle);
        }
    }

    void StartupManagedSystem(void* Handle, const FSystemContext* Context)
    {
        if (IsInitialized() && StartupEntitySystem && Handle)
        {
            StartupEntitySystem.Get()(Handle, const_cast<void*>(reinterpret_cast<const void*>(Context)));
        }
    }

    void TickManagedSystem(void* Handle, const FSystemContext* Context)
    {
        if (IsInitialized() && TickEntitySystem && Handle)
        {
            TickEntitySystem.Get()(Handle, const_cast<void*>(reinterpret_cast<const void*>(Context)));
        }
    }
}
