#include "RuntimePCH.h"
#include "Component.h"

#include "Core/Object/Class.h"
#include "Core/Serialization/Archiver.h"
#include "Memory/Memory.h"
#include "World/ECS/CommandBus.h"

namespace Lumina
{
    namespace
    {
        const ECS::FComponentTypeInfo& InfoOf(const FComponentOps& Ops)
        {
            return ECS::FComponentTypeRegistry::Get().GetInfo(Ops.GetTypeID());
        }

        // A detached value, aligned for its type. A tag still gets a byte, so New never hands back null.
        void* AllocateValue(const ECS::FComponentTypeInfo& Info)
        {
            const size_t Size = Info.Size != 0 ? Info.Size : 1u;
            const size_t Alignment = Info.Alignment != 0 ? Info.Alignment : 1u;
            void* Value = Memory::Malloc(Size, Alignment);
            if (Info.Size != 0 && Info.DefaultConstruct != nullptr)
            {
                Info.DefaultConstruct(Info, Value);
            }
            return Value;
        }

        void FreeValue(const ECS::FComponentTypeInfo& Info, void* Value)
        {
            if (Value == nullptr)
            {
                return;
            }
            if (Info.Size != 0 && !Info.bTriviallyDestructible && Info.Destruct != nullptr)
            {
                Info.Destruct(Info, Value);
            }
            Memory::Free(Value);
        }

        // Owns a staged value until its command runs, so a discarded command still frees it.
        struct FStagedComponent
        {
            const FComponentOps* Ops;
            void* Value;

            explicit FStagedComponent(const FComponentOps* InOps) : Ops(InOps), Value(InOps->New()) {}
            FStagedComponent(FStagedComponent&& Other) noexcept : Ops(Other.Ops), Value(Other.Value) { Other.Value = nullptr; }
            ~FStagedComponent() { if (Value != nullptr) { Ops->Delete(Value); } }

            FStagedComponent(const FStagedComponent&) = delete;
            FStagedComponent& operator=(const FStagedComponent&) = delete;
            FStagedComponent& operator=(FStagedComponent&&) = delete;
        };

        ECS::FComponentSignal& SignalOf(ECS::FSparseSet& Storage, EComponentSignal Kind)
        {
            switch (Kind)
            {
                case EComponentSignal::Destroy: return Storage.Signals.OnDestroy;
                case EComponentSignal::Update:  return Storage.Signals.OnUpdate;
                default:                        return Storage.Signals.OnConstruct;
            }
        }
    }

    void* FComponentOps::Get(ECS::FRegistry& Registry, ECS::FEntity Entity) const
    {
        ECS::FSparseSet* Storage = Registry.FindStorage(GetTypeID());
        if (Storage == nullptr)
        {
            return nullptr;
        }
        Storage->MarkPointersEscaped();
        return Storage->GetRawForWrite(Entity);
    }

    void* FComponentOps::GetUnmarked(ECS::FRegistry& Registry, ECS::FEntity Entity) const
    {
        const ECS::FSparseSet* Storage = Registry.FindStorage(GetTypeID());
        return Storage != nullptr ? Storage->GetRaw(Entity) : nullptr;
    }

    int32 FComponentOps::Has(ECS::FRegistry& Registry, ECS::FEntity Entity) const
    {
        const ECS::FSparseSet* Storage = Registry.FindStorage(GetTypeID());
        return Storage != nullptr && Storage->Contains(Entity) ? 1 : 0;
    }

    void* FComponentOps::Emplace(ECS::FRegistry& Registry, ECS::FEntity Entity) const
    {
        void* Value = Registry.GetOrEmplaceRaw(GetTypeID(), Entity);
        Registry.AssureStorage(GetTypeID()).MarkPointersEscaped();
        return Value;
    }

    int32 FComponentOps::Remove(ECS::FRegistry& Registry, ECS::FEntity Entity) const
    {
        return Registry.RemoveRaw(GetTypeID(), Entity) ? 1 : 0;
    }

    void* FComponentOps::New() const
    {
        return AllocateValue(InfoOf(*this));
    }

    void FComponentOps::Delete(void* Value) const
    {
        FreeValue(InfoOf(*this), Value);
    }

    void* FComponentOps::EmplaceCopy(ECS::FRegistry& Registry, ECS::FEntity Entity, const void* Source) const
    {
        return Registry.EmplaceOrReplaceRaw(GetTypeID(), Entity, Source);
    }

    void* FComponentOps::EmplaceDefault(ECS::FRegistry& Registry, ECS::FEntity Entity) const
    {
        return Registry.EmplaceOrReplaceRaw(GetTypeID(), Entity, nullptr);
    }

    void* FComponentOps::StageEmplace(ECS::FCommandBus& Commands, ECS::FEntity Entity) const
    {
        FStagedComponent Staged(this);
        void* Value = InfoOf(*this).bEmpty ? nullptr : Staged.Value;
        Commands.Enqueue([Entity, Staged = Move(Staged)](ECS::FRegistry& InRegistry)
        {
            if (InRegistry.IsValid(Entity))
            {
                Staged.Ops->EmplaceCopy(InRegistry, Entity, Staged.Value);
            }
        });
        return Value;
    }

    void* FComponentOps::EmplaceSerialized(ECS::FRegistry& Registry, ECS::FEntity Entity, FArchive& Ar) const
    {
        const ECS::FComponentTypeInfo& Info = InfoOf(*this);
        CStruct* Struct = Info.GetStruct();
        if (Info.bEmpty || Info.Size == 0 || Struct == nullptr)
        {
            return Registry.GetOrEmplaceRaw(GetTypeID(), Entity);
        }

        void* Value = AllocateValue(Info);
        Struct->SerializeProperties(Ar, Value);
        void* Placed = Registry.EmplaceOrReplaceRaw(GetTypeID(), Entity, Value);
        FreeValue(Info, Value);
        return Placed;
    }

    void FComponentOps::ConnectSignal(ECS::FRegistry& Registry, EComponentSignal Kind, FManagedSignalListener* Listener) const
    {
        Listener->Connection = SignalOf(Registry.AssureStorage(GetTypeID()), Kind).template Connect<&ManagedSignalTrampoline>(Listener);
    }

    void FComponentOps::DisconnectSignal(ECS::FRegistry&, EComponentSignal, FManagedSignalListener* Listener) const
    {
        Listener->Connection.Release();
    }

    void FComponentOps::Patch(ECS::FRegistry& Registry, ECS::FEntity Entity) const
    {
        Registry.PatchRaw(GetTypeID(), Entity);
    }

    CStruct* FComponentOps::StaticStruct() const
    {
        return InfoOf(*this).GetStruct();
    }

    void FComponentOps::Assure(ECS::FRegistry& Registry) const
    {
        (void)Registry.AssureStorage(GetTypeID());
    }
}
