#include "RuntimePCH.h"
#include "GameplayMessageBus.h"
#include "Core/Delegates/CoreDelegates.h"
#include "Core/Delegates/ScriptDelegate.h"
#include "Core/Object/Class.h"
#include "Memory/Memory.h"
#include "World/ECS/CommandBus.h"
#include "World/ECS/Registry.h"
#include <algorithm>

namespace Lumina
{
    namespace
    {
        constexpr uint32 kMaxTagDepth = 32;

        void FreeManagedPayload(const FGameplayMessagePayloadRef& Payload)
        {
            if (Payload.Kind == EGameplayMessagePayload::Managed && Payload.Data != nullptr && GFreeManagedDelegateContext != nullptr)
            {
                GFreeManagedDelegateContext(const_cast<void*>(Payload.Data));
            }
        }

        // An owned copy of a payload, for a send that waits for the command bus flush.
        struct FDeferredPayload
        {
            FGameplayMessagePayloadRef Ref;
            void*                      Memory = nullptr;

            explicit FDeferredPayload(const FGameplayMessagePayloadRef& Source)
                : Ref(Source)
            {
                if (Source.Data == nullptr || Source.Kind == EGameplayMessagePayload::Managed)
                {
                    return;
                }

                if (Source.Kind == EGameplayMessagePayload::Struct && Source.Struct != nullptr)
                {
                    Memory = Memory::Malloc(Source.Struct->GetSize(), Math::Max<size_t>(Source.Struct->GetAlignment(), 16));
                    Source.Struct->InitializeStruct(Memory);
                    Source.Struct->CopyStruct(Memory, Source.Data);
                }
                else
                {
                    Memory = Memory::Malloc(Math::Max<size_t>(Source.Size, 1), 16);
                    Memory::Memcpy(Memory, Source.Data, Source.Size);
                }
                Ref.Data = Memory;
            }

            FDeferredPayload(FDeferredPayload&& Other) noexcept
                : Ref(Other.Ref), Memory(Other.Memory)
            {
                Other.Memory   = nullptr;
                Other.Ref.Data = nullptr;
                Other.Ref.Kind = EGameplayMessagePayload::None;
            }

            FDeferredPayload(const FDeferredPayload&) = delete;
            FDeferredPayload& operator = (const FDeferredPayload&) = delete;
            FDeferredPayload& operator = (FDeferredPayload&&) = delete;

            ~FDeferredPayload()
            {
                if (Memory != nullptr)
                {
                    if (Ref.Kind == EGameplayMessagePayload::Struct && Ref.Struct != nullptr)
                    {
                        Ref.Struct->DestroyStruct(Memory);
                    }
                    Memory::Free(Memory);
                }
                // A send that never ran still owns its managed handle.
                FreeManagedPayload(Ref);
            }
        };

        void AddId(THashMap<uint32, TVector<uint64>>& Index, uint32 Key, uint64 Id)
        {
            Index[Key].push_back(Id);
        }

        void RemoveId(THashMap<uint32, TVector<uint64>>& Index, uint32 Key, uint64 Id)
        {
            auto It = Index.find(Key);
            if (It == Index.end())
            {
                return;
            }
            TVector<uint64>& Ids = It->second;
            for (size_t i = 0; i < Ids.size(); ++i)
            {
                if (Ids[i] == Id)
                {
                    Ids.erase(Ids.begin() + (ptrdiff_t)i);
                    break;
                }
            }
            if (Ids.empty())
            {
                Index.erase(It);
            }
        }
    }

    uint64 MakeGameplayMessageType(FName TypeName)
    {
        // The two lowest keys mean any type and a managed reference, so a hash may not land on them.
        const uint64 Hash = TypeName.GetStableHash();
        return Hash <= kManagedGameplayMessageType ? Hash + 2 : Hash;
    }

    FGameplayMessageBus::FGameplayMessageBus(ECS::FRegistry& InRegistry, ECS::FCommandBus& InCommands)
        : Registry(InRegistry)
        , Commands(InCommands)
    {
        ScriptReloadHandle = FCoreDelegates::OnScriptsWillReload.AddLambda([this] { RemoveManagedListeners(); });
    }

    FGameplayMessageBus::~FGameplayMessageBus()
    {
        FCoreDelegates::OnScriptsWillReload.Remove(ScriptReloadHandle);
        Clear();
    }

    void FGameplayMessageBus::RemoveManagedListeners()
    {
        {
            FScopeLock Lock(Mutex);
            TVector<uint64> Managed;
            for (const auto& [Id, Listener] : ById)
            {
                if (Listener.bManaged)
                {
                    Managed.push_back(Id);
                }
            }
            for (uint64 Id : Managed)
            {
                RemoveLocked(Id);
            }
        }
        DestroyRetired();
    }

    uint64 FGameplayMessageBus::Subscribe(const FSubscribeParams& Params)
    {
        if (Params.Invoke == nullptr || Params.Channel == 0)
        {
            if (Params.Destroy != nullptr)
            {
                Params.Destroy(Params.Context);
            }
            return 0;
        }

        FListener Listener;
        Listener.Type     = Params.Type;
        Listener.Channel  = Params.Channel;
        Listener.Owner    = Params.Owner;
        Listener.Match    = Params.Match;
        Listener.Invoke   = Params.Invoke;
        Listener.Context  = Params.Context;
        Listener.Destroy  = Params.Destroy;
        Listener.bManaged = Params.bManaged;

        FScopeLock Lock(Mutex);
        const uint64 Id = NextId++;
        ById.emplace(Id, Listener);
        if (Params.Owner.IsNull())
        {
            AddId(ByChannel, Params.Channel, Id);
        }
        else
        {
            AddId(ByOwner, Params.Owner.GetPacked(), Id);
        }
        return Id;
    }

    bool FGameplayMessageBus::RemoveLocked(uint64 Id)
    {
        auto It = ById.find(Id);
        if (It == ById.end())
        {
            return false;
        }

        const FListener Listener = It->second;
        ById.erase(It);
        if (Listener.Owner.IsNull())
        {
            RemoveId(ByChannel, Listener.Channel, Id);
        }
        else
        {
            RemoveId(ByOwner, Listener.Owner.GetPacked(), Id);
        }
        Retired.push_back(Listener);
        return true;
    }

    bool FGameplayMessageBus::Unsubscribe(uint64 Id)
    {
        bool bRemoved;
        {
            FScopeLock Lock(Mutex);
            bRemoved = RemoveLocked(Id);
        }
        DestroyRetired();
        return bRemoved;
    }

    void FGameplayMessageBus::UnsubscribeAll(ECS::FEntity Owner)
    {
        {
            FScopeLock Lock(Mutex);
            auto It = ByOwner.find(Owner.GetPacked());
            if (It != ByOwner.end())
            {
                const TVector<uint64> Ids = It->second;
                for (uint64 Id : Ids)
                {
                    RemoveLocked(Id);
                }
            }
        }
        DestroyRetired();
    }

    void FGameplayMessageBus::Clear()
    {
        {
            FScopeLock Lock(Mutex);
            for (auto& [Id, Listener] : ById)
            {
                Retired.push_back(Listener);
            }
            ById.clear();
            ByChannel.clear();
            ByOwner.clear();
        }
        DestroyRetired();
    }

    void FGameplayMessageBus::PruneDeadOwners()
    {
        TVector<uint32> Dead;
        {
            FScopeLock Lock(Mutex);
            for (const auto& [Packed, Ids] : ByOwner)
            {
                if (!Registry.IsValid(ECS::FEntity::FromPacked(Packed)))
                {
                    Dead.push_back(Packed);
                }
            }
        }
        for (uint32 Packed : Dead)
        {
            UnsubscribeAll(ECS::FEntity::FromPacked(Packed));
        }
    }

    uint32 FGameplayMessageBus::NumListeners() const
    {
        FScopeLock Lock(Mutex);
        return (uint32)ById.size();
    }

    void FGameplayMessageBus::DestroyRetired()
    {
        TVector<FListener> Destroy;
        {
            FScopeLock Lock(Mutex);
            if (DispatchDepth != 0 || Retired.empty())
            {
                return;
            }
            Destroy.swap(Retired);
        }
        for (const FListener& Listener : Destroy)
        {
            if (Listener.Destroy != nullptr)
            {
                Listener.Destroy(Listener.Context);
            }
        }
    }

    void FGameplayMessageBus::Send(EGameplayMessageRoute Route, ECS::FEntity Entity, uint32 Channel, const FGameplayMessagePayloadRef& Payload, bool bIncludeSelf)
    {
        if (Channel == 0 || (Route != EGameplayMessageRoute::Broadcast && Entity.IsNull()))
        {
            FreeManagedPayload(Payload);
            return;
        }

        if (!ECS::FCommandBus::ShouldDefer())
        {
            Deliver(Route, Entity, Channel, Payload, bIncludeSelf);
            FreeManagedPayload(Payload);
            return;
        }

        Commands.Enqueue([this, Route, Entity, Channel, bIncludeSelf, Copy = FDeferredPayload(Payload)]() mutable
        {
            Deliver(Route, Entity, Channel, Copy.Ref, bIncludeSelf);
        });
    }

    void FGameplayMessageBus::Deliver(EGameplayMessageRoute Route, ECS::FEntity Entity, uint32 Channel, const FGameplayMessagePayloadRef& Payload, bool bIncludeSelf)
    {
        LUMINA_PROFILE_SCOPE();

        TVector<uint32> ChannelChain;
        for (uint32 Tag = Channel; Tag != 0 && ChannelChain.size() < kMaxTagDepth; Tag = FGameplayTagRegistry::Get().GetParent(Tag))
        {
            ChannelChain.push_back(Tag);
        }

        FGameplayMessage Message;
        Message.Type    = Payload.Type;
        Message.Payload = Payload.Data;
        Message.Size    = Payload.Size;
        Message.Kind    = Payload.Kind;
        Message.Channel = Channel;
        Message.Source  = Entity.GetPacked();

        {
            FScopeLock Lock(Mutex);
            ++DispatchDepth;
        }

        TVector<uint64> Ids;
        if (Route == EGameplayMessageRoute::Broadcast)
        {
            for (uint32 Tag : ChannelChain)
            {
                Ids.clear();
                {
                    FScopeLock Lock(Mutex);
                    auto It = ByChannel.find(Tag);
                    if (It != ByChannel.end())
                    {
                        Ids = It->second;
                    }
                }
                Message.Channel = Tag;
                DeliverTo(Ids, Message, ChannelChain, false);
            }
        }
        else
        {
            TVector<ECS::FEntity> Nodes;
            const ECS::FHierarchy& Hierarchy = Registry.GetHierarchy();
            if (Route == EGameplayMessageRoute::Up)
            {
                for (ECS::FEntity Node = Entity; !Node.IsNull(); Node = Hierarchy.GetParent(Node))
                {
                    Nodes.push_back(Node);
                }
            }
            else if (Route == EGameplayMessageRoute::Down)
            {
                Nodes.push_back(Entity);
                Hierarchy.ForEachDescendant(Entity, [&](ECS::FEntity Node) { Nodes.push_back(Node); });
            }
            else
            {
                Nodes.push_back(Entity);
            }

            const size_t First = (bIncludeSelf || Route == EGameplayMessageRoute::To) ? 0 : 1;
            for (size_t i = First; i < Nodes.size(); ++i)
            {
                Ids.clear();
                {
                    FScopeLock Lock(Mutex);
                    auto It = ByOwner.find(Nodes[i].GetPacked());
                    if (It != ByOwner.end())
                    {
                        Ids = It->second;
                    }
                }
                Message.Target = Nodes[i].GetPacked();
                if (!Ids.empty() && DeliverTo(Ids, Message, ChannelChain, true))
                {
                    break;
                }
            }
        }

        {
            FScopeLock Lock(Mutex);
            --DispatchDepth;
        }
        DestroyRetired();
    }

    bool FGameplayMessageBus::DeliverTo(const TVector<uint64>& Ids, FGameplayMessage& Message, const TVector<uint32>& ChannelChain, bool bRouted)
    {
        for (uint64 Id : Ids)
        {
            FListener Listener;
            {
                FScopeLock Lock(Mutex);
                auto It = ById.find(Id);
                if (It == ById.end())
                {
                    continue;
                }
                Listener = It->second;
            }

            if (Listener.Type != 0 && Listener.Type != Message.Type)
            {
                continue;
            }

            // A broadcast walks the channel's ancestors itself, so here it is only the exact level that matters.
            const uint32 SentChannel = ChannelChain.front();
            bool bReaches;
            if (bRouted)
            {
                bReaches = Listener.Match == EGameplayTagMatch::Exact
                    ? Listener.Channel == SentChannel
                    : std::find(ChannelChain.begin(), ChannelChain.end(), Listener.Channel) != ChannelChain.end();
            }
            else
            {
                bReaches = Listener.Match == EGameplayTagMatch::Partial || Message.Channel == SentChannel;
            }
            if (!bReaches)
            {
                continue;
            }

            FGameplayMessage Delivered = Message;
            Delivered.Channel = SentChannel;
            if (Listener.Invoke(Listener.Context, &Delivered) != 0 && bRouted)
            {
                return true;
            }
        }
        return false;
    }
}
