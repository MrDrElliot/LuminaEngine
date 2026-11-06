#include <gtest/gtest.h>

#include "Components/HealthComponent.h"
#include "Core/Object/ObjectCore.h"
#include "World/World.h"
#include "WorldTestAccess.h"

using namespace Lumina;

namespace
{
    const ECS::FEntity Attacker = static_cast<ECS::FEntity>(7u);

    struct FHealthLog
    {
        TVector<SHealthChangedEvent> Changed;
        TVector<bool>                DeadWhenChanged;
        int32                        Died = 0;

        explicit FHealthLog(SHealthComponent& Health)
        {
            Health.OnHealthChanged.AddLambda([this, &Health](const SHealthChangedEvent& Event)
            {
                Changed.Add(Event);
                DeadWhenChanged.Add(Health.IsDead());
            });
            Health.OnDied.AddLambda([this](const SHealthChangedEvent&) { ++Died; });
        }
    };

    // A bare world running only CHealthSystem, found by name since the class is not exported.
    struct FRegenWorld
    {
        TStrongObjectPtr<CWorld> World;
        ECS::FEntity             Entity;

        explicit FRegenWorld(EWorldType Type = EWorldType::Game)
        {
            RegisterPendingTypes();
            World = TStrongObjectPtr<CWorld>(NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient));
            FWorldTestAccess::SetWorldType(*World, Type);
            ECS::FRegistry& Registry = World->GetEntityRegistry();
            Registry.Ctx().Emplace<CWorld*>(World.Get());

            FWorldTestAccess::KeepOnlySystems(*World, { "CHealthSystem" });

            Entity = Registry.Create();
            SHealthComponent& Health = Registry.Emplace<SHealthComponent>(Entity);
            Health.RegenPerSecond = 10.0f;
            Health.RegenDelay = 1.0f;
        }

        SHealthComponent& Health() const { return World->GetEntityRegistry().Get<SHealthComponent>(Entity); }

        float Tick() const
        {
            FWorldTestAccess::TickStage(*World, EUpdateStage::PrePhysics, 0.5);
            return Health().Health;
        }
    };

    // Two health entities where the first despawns itself from OnHealthChanged once it dies.
    struct FDespawnOnDeath
    {
        ECS::FRegistry Registry;
        ECS::FEntity   Doomed;
        ECS::FEntity   Bystander;
        int32          DoomedDied = 0;
        int32          BystanderDied = 0;

        FDespawnOnDeath()
        {
            RegisterPendingTypes();
            Doomed = Registry.Create();
            Bystander = Registry.Create();

            SHealthComponent& DoomedHealth = Registry.Emplace<SHealthComponent>(Doomed);
            DoomedHealth.OnDied.AddLambda([this](const SHealthChangedEvent&) { ++DoomedDied; });
            DoomedHealth.OnHealthChanged.AddLambda([this](const SHealthChangedEvent& Event)
            {
                if (Event.Health <= 0.0f)
                {
                    Registry.Destroy(Doomed);
                }
            });
            Registry.Emplace<SHealthComponent>(Bystander).OnDied.AddLambda([this](const SHealthChangedEvent&) { ++BystanderDied; });
        }

        SHealthComponent& DoomedHealth() { return Registry.Get<SHealthComponent>(Doomed); }
    };
}

// The doomed entity's own OnDied is skipped by design, since its component and delegate are already gone.
TEST(HealthComponent, ALethalHitThatDespawnsTheEntityDoesNotKillAnother)
{
    FDespawnOnDeath Fixture;

    EXPECT_FLOAT_EQ(Fixture.DoomedHealth().ApplyDamage(150.0f, Attacker), 0.0f);
    EXPECT_FALSE(Fixture.Registry.IsValid(Fixture.Doomed));
    EXPECT_EQ(Fixture.BystanderDied, 0);
    EXPECT_EQ(Fixture.DoomedDied, 0);
    EXPECT_FALSE(Fixture.Registry.Get<SHealthComponent>(Fixture.Bystander).IsDead());
}

TEST(HealthComponent, AKillThatDespawnsTheEntityDoesNotKillAnother)
{
    FDespawnOnDeath Fixture;

    Fixture.DoomedHealth().Kill(Attacker);
    EXPECT_FALSE(Fixture.Registry.IsValid(Fixture.Doomed));
    EXPECT_EQ(Fixture.BystanderDied, 0);
    EXPECT_EQ(Fixture.DoomedDied, 0);
    EXPECT_FALSE(Fixture.Registry.Get<SHealthComponent>(Fixture.Bystander).IsDead());
}

TEST(HealthComponent, DamageClampsAtZeroAndLatchesDeathBeforeBroadcasting)
{
    SHealthComponent Health;
    FHealthLog Log(Health);

    EXPECT_FLOAT_EQ(Health.ApplyDamage(250.0f, Attacker), 0.0f);
    EXPECT_TRUE(Health.IsDead());
    ASSERT_EQ(Log.Changed.Num(), 1);
    EXPECT_FLOAT_EQ(Log.Changed[0].Delta, -100.0f) << "the delta is what was applied after clamping";
    EXPECT_EQ(Log.Changed[0].Instigator, Attacker);
    EXPECT_TRUE(Log.DeadWhenChanged[0]) << "OnHealthChanged listeners already see the death";
    EXPECT_EQ(Log.Died, 1);
}

TEST(HealthComponent, DamageResetsTheRegenCooldownAndIgnoresNonPositiveAmounts)
{
    SHealthComponent Health;
    FHealthLog Log(Health);

    EXPECT_FLOAT_EQ(Health.ApplyDamage(0.0f, Attacker), 100.0f);
    EXPECT_FLOAT_EQ(Health.ApplyDamage(-5.0f, Attacker), 100.0f);
    EXPECT_TRUE(Log.Changed.IsEmpty());

    EXPECT_FLOAT_EQ(Health.ApplyDamage(30.0f, Attacker), 70.0f);
    EXPECT_FLOAT_EQ(Health.RegenCooldown, Health.RegenDelay);
    EXPECT_FALSE(Health.IsDead());
    EXPECT_EQ(Log.Died, 0);
}

TEST(HealthComponent, AListenerDamagingAgainAfterTheKillingBlowDoesNotDieTwice)
{
    SHealthComponent Health;
    FHealthLog Log(Health);
    Health.OnHealthChanged.AddLambda([&Health](const SHealthChangedEvent&)
    {
        Health.ApplyDamage(10.0f, Attacker);
        Health.Kill(Attacker);
    });

    Health.ApplyDamage(100.0f, Attacker);
    EXPECT_EQ(Log.Changed.Num(), 1);
    EXPECT_EQ(Log.Died, 1);
}

TEST(HealthComponent, AListenerLandingTheKillingBlowDiesOnce)
{
    SHealthComponent Health;
    FHealthLog Log(Health);
    bool bStruckBack = false;
    Health.OnHealthChanged.AddLambda([&](const SHealthChangedEvent&)
    {
        if (!bStruckBack)
        {
            bStruckBack = true;
            Health.ApplyDamage(60.0f, Attacker);
        }
    });

    Health.ApplyDamage(60.0f, Attacker);
    EXPECT_TRUE(Health.IsDead());
    EXPECT_EQ(Log.Changed.Num(), 2);
    EXPECT_EQ(Log.Died, 1) << "the outer non-lethal hit must not report the nested death again";
}

TEST(HealthComponent, HealClampsAtMaxAndSkipsZeroChanges)
{
    SHealthComponent Health;
    FHealthLog Log(Health);

    EXPECT_FLOAT_EQ(Health.Heal(10.0f, Attacker), 100.0f);
    EXPECT_TRUE(Log.Changed.IsEmpty()) << "healing at full health changes nothing";

    Health.ApplyDamage(30.0f, Attacker);
    EXPECT_FLOAT_EQ(Health.Heal(0.0f, Attacker), 70.0f);
    EXPECT_FLOAT_EQ(Health.Heal(50.0f, Attacker), 100.0f);
    ASSERT_EQ(Log.Changed.Num(), 2);
    EXPECT_FLOAT_EQ(Log.Changed[1].Delta, 30.0f);
}

TEST(HealthComponent, HealDoesNothingWhileDead)
{
    SHealthComponent Health;
    Health.Kill(Attacker);
    FHealthLog Log(Health);

    EXPECT_FLOAT_EQ(Health.Heal(50.0f, Attacker), 0.0f);
    EXPECT_TRUE(Health.IsDead());
    EXPECT_TRUE(Log.Changed.IsEmpty());
}

TEST(HealthComponent, KillFiresBothEventsOnce)
{
    SHealthComponent Health;
    FHealthLog Log(Health);

    Health.Kill(Attacker);
    Health.Kill(Attacker);
    Health.ApplyDamage(10.0f, Attacker);

    EXPECT_FLOAT_EQ(Health.Health, 0.0f);
    EXPECT_FLOAT_EQ(Health.RegenCooldown, Health.RegenDelay);
    ASSERT_EQ(Log.Changed.Num(), 1);
    EXPECT_FLOAT_EQ(Log.Changed[0].Delta, -100.0f);
    EXPECT_TRUE(Log.DeadWhenChanged[0]);
    EXPECT_EQ(Log.Died, 1);
}

TEST(HealthComponent, ReviveRestoresHealthAndCanDieAgain)
{
    SHealthComponent Health;
    FHealthLog Log(Health);

    Health.Kill(Attacker);
    Health.Revive(0.0f);
    EXPECT_FALSE(Health.IsDead());
    EXPECT_FLOAT_EQ(Health.Health, 100.0f);
    EXPECT_FLOAT_EQ(Health.RegenCooldown, 0.0f);
    EXPECT_EQ(Log.Died, 1) << "Revive fires OnHealthChanged only";

    Health.Kill(Attacker);
    Health.Revive(-5.0f);
    EXPECT_FLOAT_EQ(Health.Health, 100.0f);

    Health.Revive(30.0f);
    EXPECT_FLOAT_EQ(Health.Health, 30.0f);
    Health.Revive(500.0f);
    EXPECT_FLOAT_EQ(Health.Health, 100.0f);

    EXPECT_EQ(Log.Died, 2);
    EXPECT_FLOAT_EQ(Log.Changed.Last().Delta, 70.0f);
}

TEST(HealthSystem, RegenWaitsOutTheCooldownThenHeals)
{
    FRegenWorld Fixture;
    ASSERT_EQ(Fixture.World->Systems.Num(), 1) << "CHealthSystem was not found";
    Fixture.Health().ApplyDamage(50.0f, Attacker);

    EXPECT_FLOAT_EQ(Fixture.Tick(), 50.0f);
    EXPECT_FLOAT_EQ(Fixture.Health().RegenCooldown, 0.5f);
    EXPECT_FLOAT_EQ(Fixture.Tick(), 50.0f) << "the tick that ends the cooldown does not heal yet";
    EXPECT_FLOAT_EQ(Fixture.Tick(), 55.0f);
    EXPECT_FLOAT_EQ(Fixture.Tick(), 60.0f);
}

TEST(HealthSystem, RegenStopsAtFullHealth)
{
    FRegenWorld Fixture;
    Fixture.Health().Health = 98.0f;
    FHealthLog Log(Fixture.Health());

    EXPECT_FLOAT_EQ(Fixture.Tick(), 100.0f);
    EXPECT_FLOAT_EQ(Fixture.Tick(), 100.0f);
    EXPECT_EQ(Log.Changed.Num(), 1) << "a full component is skipped, not healed by zero";
}

TEST(HealthSystem, EditorWorldsDoNotRegen)
{
    FRegenWorld Fixture(EWorldType::Editor);
    Fixture.Health().Health = 50.0f;

    EXPECT_FLOAT_EQ(Fixture.Tick(), 50.0f);
    EXPECT_FLOAT_EQ(Fixture.Tick(), 50.0f);
}
