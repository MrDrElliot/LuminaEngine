#include "RuntimePCH.h"
#include "LifetimeProbeTestTypes.h"

#include <atomic>
#include "Log/Log.h"

namespace Lumina::LifetimeProbeTestTypesInternal
{
    constexpr uint64 kAliveMagic = 0x11FE7173A11FE001ull;
    constexpr uint64 kDeadMagic  = 0xDEADDEADDEAD0001ull;

    std::atomic<int64>& LiveCount()
    {
        static std::atomic<int64> Count{ 0 };
        return Count;
    }

    std::atomic<int64>& ErrorCount()
    {
        static std::atomic<int64> Count{ 0 };
        return Count;
    }
}

namespace Lumina
{
    FLifetimeProbe::FLifetimeProbe() noexcept
    {
        Arm();
    }

    FLifetimeProbe::FLifetimeProbe(const FLifetimeProbe& Other) noexcept
    {
        Other.Check("copied from");
        Arm();
    }

    FLifetimeProbe::FLifetimeProbe(FLifetimeProbe&& Other) noexcept
    {
        Other.Check("moved from");
        Arm();
    }

    FLifetimeProbe& FLifetimeProbe::operator=(const FLifetimeProbe& Other) noexcept
    {
        Check("copy assigned to");
        Other.Check("copy assigned from");
        return *this;
    }

    FLifetimeProbe& FLifetimeProbe::operator=(FLifetimeProbe&& Other) noexcept
    {
        Check("move assigned to");
        Other.Check("move assigned from");
        return *this;
    }

    FLifetimeProbe::~FLifetimeProbe()
    {
        Check("destroyed");
        Self = nullptr;
        Magic = LifetimeProbeTestTypesInternal::kDeadMagic;
        LifetimeProbeTestTypesInternal::LiveCount().fetch_sub(1, std::memory_order_relaxed);
    }

    int64 FLifetimeProbe::GetLiveCount()
    {
        return LifetimeProbeTestTypesInternal::LiveCount().load(std::memory_order_relaxed);
    }

    int64 FLifetimeProbe::GetErrorCount()
    {
        return LifetimeProbeTestTypesInternal::ErrorCount().load(std::memory_order_relaxed);
    }

    void FLifetimeProbe::Arm()
    {
        Self = this;
        Magic = LifetimeProbeTestTypesInternal::kAliveMagic;
        LifetimeProbeTestTypesInternal::LiveCount().fetch_add(1, std::memory_order_relaxed);
    }

    void FLifetimeProbe::Check(const char* Operation) const
    {
        if (Self == this && Magic == LifetimeProbeTestTypesInternal::kAliveMagic)
        {
            return;
        }

        LifetimeProbeTestTypesInternal::ErrorCount().fetch_add(1, std::memory_order_relaxed);
        const char* Why = Magic == LifetimeProbeTestTypesInternal::kDeadMagic ? "after it was destroyed"
            : Magic == LifetimeProbeTestTypesInternal::kAliveMagic ? "after its bytes were moved without a move constructor"
            : "without ever being constructed";
        LOG_ERROR("Lifetime probe at {} {} {} (it records {}).", (const void*)this, Operation, Why, (const void*)Self);
    }
}
