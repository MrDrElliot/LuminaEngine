#include <gtest/gtest.h>

#include "Containers/Name.h"
#include "Containers/Vector.h"
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

    // A parent holding the only other reference to its child, the shape a release-then-adopt assign breaks.
    CObjectRefTest* NewParentOwningChild(CObject*& OutChild)
    {
        auto* Parent = NewObject<CObjectRefTest>(nullptr, NAME_None, FGuid::New(), OF_Transient);
        OutChild = NewTransientTestObject();
        Parent->Child = OutChild;
        return Parent;
    }
}

// The shape before the fix, kept so the pinned test above cannot pass vacuously.
TEST(CObjectLifetime, DestroyingAnUnpinnedListFreesAnEntryBeforeItsTurn)
{
    FScopedStaleReferenceTolerance Tolerance;

    CObject* Child = nullptr;
    CObjectRefTest* Parent = NewParentOwningChild(Child);
    ASSERT_NE(Child, nullptr);

    TWeakObjectPtr<CObject> WeakChild = Child;

    EXPECT_TRUE(GObjectArray.ConditionalDestroy(Parent));
    EXPECT_FALSE(WeakChild.IsValid()) << "releasing the parent already freed the child";
    EXPECT_FALSE(GObjectArray.ConditionalDestroy(Child)) << "the child's own turn lands on a freed slot";
}

// The import teardown shape, where a later entry holds the only other reference to an earlier one.
TEST(CObjectLifetime, PoppingAPinnedListDestroysEachEntryExactlyOnce)
{
    CObject* Child = nullptr;
    CObjectRefTest* Parent = NewParentOwningChild(Child);
    ASSERT_NE(Child, nullptr);

    TWeakObjectPtr<CObject> WeakChild = Child;
    TWeakObjectPtr<CObject> WeakParent = Parent;

    TVector<TObjectPtr<CObject>> Created;
    Created.push_back(Child);
    Created.push_back(Parent);

    Created.pop_back();
    EXPECT_FALSE(WeakParent.IsValid());
    ASSERT_EQ(Created.size(), 1u);
    EXPECT_TRUE(Created.back().IsValid())
        << "the child's own pin must outlive the parent that held the other reference to it";
    EXPECT_EQ(Created.back().Get(), Child);

    Created.pop_back();
    EXPECT_FALSE(WeakChild.IsValid());
}

// Assigning out of the object being released has to adopt before it drops, or the source is freed mid-read.
TEST(CObjectLifetime, AssigningOutOfTheReleasedObjectKeepsTheNewTarget)
{
    CObject* Child = nullptr;
    TObjectPtr<CObject> Ref = NewParentOwningChild(Child);
    ASSERT_NE(Child, nullptr);

    Ref = static_cast<CObjectRefTest*>(Ref.Get())->Child;

    EXPECT_EQ(Ref.Get(), Child) << "the child must survive the parent that held the only other reference";
    EXPECT_TRUE(Ref.IsValid());
}

TEST(CObjectLifetime, MoveAssigningOutOfTheReleasedObjectKeepsTheNewTarget)
{
    CObject* Child = nullptr;
    TObjectPtr<CObject> Ref = NewParentOwningChild(Child);
    ASSERT_NE(Child, nullptr);

    Ref = Move(static_cast<CObjectRefTest*>(Ref.Get())->Child);

    EXPECT_EQ(Ref.Get(), Child);
    EXPECT_TRUE(Ref.IsValid());
}

TEST(CObjectLifetime, RawAssigningOutOfTheReleasedObjectKeepsTheNewTarget)
{
    CObject* Child = nullptr;
    TObjectPtr<CObject> Ref = NewParentOwningChild(Child);
    ASSERT_NE(Child, nullptr);

    Ref = static_cast<CObjectRefTest*>(Ref.Get())->Child.Get();

    EXPECT_EQ(Ref.Get(), Child);
    EXPECT_TRUE(Ref.IsValid());
}

// Self-assignment still has to leave the reference intact rather than release and re-adopt a dead slot.
TEST(CObjectLifetime, SelfAssignmentLeavesTheReferenceIntact)
{
    CObject* Object = NewTransientTestObject();
    TObjectPtr<CObject> Ref = Object;

    TObjectPtr<CObject>& Alias = Ref;
    Ref = Alias;
    EXPECT_EQ(Ref.Get(), Object);

    Ref = Ref.Get();
    EXPECT_EQ(Ref.Get(), Object) << "assigning a raw pointer the reference already holds must be a no-op";
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
    FScopedStaleReferenceTolerance Tolerance;

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
    FScopedStaleReferenceTolerance Tolerance;

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
    FScopedStaleReferenceTolerance Tolerance;

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

// Pins the claim's reentrancy rather than the OF_DestroyStarted guard, which it passes without.
TEST(CObjectLifetime, OnDestroyIsNotReenteredByADestroyItTriggers)
{
    CDestroyCountTest::DestroyCount = 0;

    auto* Object = NewObject<CDestroyCountTest>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Object, nullptr);
    Object->bReenterOnDestroy = true;

    Object->ConditionalBeginDestroy();

    EXPECT_EQ(CDestroyCountTest::DestroyCount, 1) << "OnDestroy ran more than once for a single destruction";
}

// A hand-marked object still destroys, because the claim and not the flag is what arbitrates the free.
TEST(CObjectLifetime, MarkingForDestroyDoesNotBlockTheActualDestroy)
{
    CDestroyCountTest::DestroyCount = 0;

    auto* Object = NewObject<CDestroyCountTest>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    ASSERT_NE(Object, nullptr);
    const FObjectHandle Handle = GObjectArray.GetHandleByObject(Object);

    // What DestroyPackage and the failed-cook path do to take an asset out of identity lookups early.
    Object->SetFlag(OF_MarkedDestroy);
    Object->ConditionalBeginDestroy();

    EXPECT_EQ(CDestroyCountTest::DestroyCount, 1) << "a pre-marked object must still be torn down";
    EXPECT_EQ(GObjectArray.ResolveHandle(Handle), nullptr) << "and its slot must be released";
}
