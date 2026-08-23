#include <gtest/gtest.h>

#include "Containers/Name.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectArray.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Scripting/ScriptableTest.h"

using namespace Lumina;

namespace
{
    CObject* NewTransientTestObject()
    {
        return NewObject(CEntityScriptTest::StaticClass(), nullptr, NAME_None, FGuid::New(), OF_Transient);
    }
}

// A strong reference carries its own array entry, so refcounting never reaches through the object.
TEST(CObjectLifetime, AStrongReferenceCarriesItsOwnSlot)
{
    EXPECT_EQ(sizeof(TObjectPtr<CObject>), 2 * sizeof(void*));
}

TEST(CObjectLifetime, AStrongReferenceKeepsItsObjectAlive)
{
    CObject* Object = NewTransientTestObject();
    ASSERT_NE(Object, nullptr);

    TObjectPtr<CObject> Owner(Object);
    EXPECT_EQ(Owner.Get(), Object);
    EXPECT_TRUE(Owner.IsValid());

    {
        TObjectPtr<CObject> Second = Owner;
        EXPECT_EQ(Second.Get(), Object) << "a copy shares the object rather than resurrecting it";
    }

    EXPECT_EQ(Owner.Get(), Object) << "dropping one of two references must not free the object";
}

// The unbalanced release below used to reach ConditionalDestroy with a freed pointer and fault there.
TEST(CObjectLifetime, AStaleStrongReferenceReadsAsNull)
{
    CObject* Doomed = NewTransientTestObject();
    ASSERT_NE(Doomed, nullptr);

    TObjectPtr<CObject> Stale(Doomed);
    ASSERT_EQ(Stale.Get(), Doomed);

    // Stands in for the extra release a lifetime bug elsewhere would perform.
    GObjectArray.ReleaseStrongRef(Doomed);

    EXPECT_EQ(Stale.Get(), nullptr) << "a reference that outlived its object must not hand back freed memory";
    EXPECT_FALSE(Stale.IsValid());
    EXPECT_FALSE(static_cast<bool>(Stale));

    // Stale destructing here logs and drops its release rather than destroying a second time.
}

TEST(CObjectLifetime, ReleasingAStaleReferenceLeavesOtherObjectsAlone)
{
    CObject* Doomed = NewTransientTestObject();
    ASSERT_NE(Doomed, nullptr);

    TObjectPtr<CObject> Stale(Doomed);
    GObjectArray.ReleaseStrongRef(Doomed);

    TObjectPtr<CObject> Bystander(NewTransientTestObject());
    ASSERT_TRUE(Bystander.IsValid());

    EXPECT_EQ(Stale.Get(), nullptr);

    // The real damage was never the read; it was the stale release being charged to another object.
    Stale.Reset();
    EXPECT_TRUE(Bystander.IsValid()) << "releasing a stale reference must not free an unrelated object";
}

// Reuse is delayed so an address-identical allocation cannot land in the slot a stale reference still names.
TEST(CObjectLifetime, AFreedSlotIsNotImmediatelyReissued)
{
    TObjectPtr<CObject> First(NewTransientTestObject());
    ASSERT_TRUE(First.IsValid());
    const int32 FirstIndex = First.GetHandle().Index;

    First.Reset();

    TObjectPtr<CObject> Second(NewTransientTestObject());
    ASSERT_TRUE(Second.IsValid());

    EXPECT_NE(Second.GetHandle().Index, FirstIndex) << "a slot handed straight back defeats the occupant check";
}

TEST(CObjectLifetime, CopyingAStaleReferenceYieldsNull)
{
    CObject* Doomed = NewTransientTestObject();
    ASSERT_NE(Doomed, nullptr);

    TObjectPtr<CObject> Stale(Doomed);
    GObjectArray.ReleaseStrongRef(Doomed);

    TObjectPtr<CObject> Reused(NewTransientTestObject());
    ASSERT_TRUE(Reused.IsValid());

    TObjectPtr<CObject> CopyOfStale = Stale;
    EXPECT_EQ(CopyOfStale.Get(), nullptr) << "copying a stale reference must not claim the new occupant";
    EXPECT_TRUE(Reused.IsValid());
}

TEST(CObjectLifetime, AWeakReferenceOutlivesItsObject)
{
    TWeakObjectPtr<CObject> Weak;
    {
        TObjectPtr<CObject> Owner(NewTransientTestObject());
        ASSERT_TRUE(Owner.IsValid());

        Weak = Owner;
        EXPECT_TRUE(Weak.IsValid());
        EXPECT_EQ(Weak.Get(), Owner.Get());

        TObjectPtr<CObject> Pinned = Weak.Lock();
        EXPECT_EQ(Pinned.Get(), Owner.Get()) << "Lock must adopt the ref the array already took";
    }

    EXPECT_FALSE(Weak.IsValid()) << "the last strong reference going away frees the object";
    EXPECT_EQ(Weak.Get(), nullptr);
    EXPECT_FALSE(Weak.Lock().IsValid());
}
