# C# Scripting Rewrite — Design

Status: **proposal**, not yet implemented.
Target: replace the current three-model C# scripting stack with one object model, one property system, and one
place where data lives.

Goals, in priority order:

1. **Safety** — no UB, no dangling native pointers reachable from C#, no silent layout drift.
2. **Efficiency** — zero allocation on the gameplay hot path, direct memory access to component and script data.
3. **Inheritance** — a user can subclass a native `CObject` type in C# and override its virtuals.

---

## 1. Why not just copy Flax

Flax solves this well, and two of its ideas are worth taking wholesale. One is not.

**Take: the object owns its managed instance.** Every Flax `ScriptingObject` carries `MGCHandle _gcHandle`,
`ScriptingTypeHandle _type`, `Guid _id`; the managed side carries `__unmanagedPtr` and `__internalId`.
`GetOrCreateManagedInstance()` returns *the* managed instance for that native object — one, forever. Reference
identity works, and nothing allocates per access.

**Take: one model for gameplay scripts.** A Flax `Script` is a `SceneObject` is a `ScriptingObject`. `OnStart`,
`OnUpdate`, `OnFixedUpdate` are ordinary `API_FUNCTION` virtuals dispatched by the same mechanism as everything
else. There is no second scripting subsystem.

**Do not take: vtable hacking.** Flax implements C#-overrides-C++ by duplicating the native vtable, replacing
slots with generated `*_ManagedWrapper` thunks, and doing `*(void**)object = Script.VTable` on construction. Slot
indices come from `GetVTableIndex()`, which parses member-function-pointer thunk *machine code* per architecture
(x86/x64/ARM64). Their own comment in `BinaryModule.cpp`:

> "Beware! Hacking vtables incoming! Undefined behaviors exploits! Low-level programming!"

Our existing generated-shim approach (`CFoo__Script : CFoo` + a minted `CClass`) achieves the same capability in
standard C++ and already composes with `NewObject` / `TSubclassOf` / editor type pickers. Keep it.

**Do not take: managed-authoritative data.** This is the important one. Flax keeps script data in C# fields and
reflects managed types directly — free for them, because **the Flax editor is C#**. Ours is C++/ImGui over
`FProperty`, and the property grid entry point is:

```cpp
FPropertyTable(void* InObject, CStruct* InType);   // Engine/Editor/Source/UI/Properties/PropertyTable.h:379
```

It needs a real type and a real buffer. Every minted `CStruct` mirror, value-blob codec, `ApplyValues`, and
migration round-trip in the tree today exists to bridge managed-authoritative data into a native-authoritative
editor. That bridge *is* the complexity.

**Therefore: keep Flax's object model, invert its data model.** Native owns the storage; C# is a typed view over
it. That is closer to Unreal than to Flax, and it is the right fit for this engine.

---

## 2. Current state

Three coexisting models for "C# touches an engine type":

| Model | Mechanism | Where |
|---|---|---|
| Reflected wrapper objects | `NativeObject` holding `IntPtr Handle`, allocated per access | `Wrapper<T>.Create` |
| Gameplay scripts | ECS carrier component + GCHandle + minted `CStruct` schema + recursive value-blob codec | `ScriptStruct.cpp`, `Serializer.cs`, `EntityScriptRuntime.cs` |
| Scriptable `CObject`s | Reflector-generated `__Script` shim + minted `CClass` + per-instance `FScriptableBridge` | `ScriptableObject.{h,cpp}` |

Rough scale: ~9.7k lines native (`Runtime/Source/Scripting`), ~15.4k lines managed (`Source/LuminaSharp`), ~3.4k
generated native thunks, 270 generated `.cs` files.

Two concrete problems this design targets:

- **Allocation on ad-hoc component access.** `EntityRegistry.cs:33` — `return Wrapper<T>.Create(Pointer);`. Every
  `Registry.Get<T>()` allocates a gen0 wrapper and returns an object with no stable identity (`==` between two
  `Get` results is false). **Corrected 2026-08-12:** this affects only the random-access path. The two paths that
  carry per-frame volume are already allocation-free — see §3 Pillar 4.
- **Two copies of script data.** Managed fields are authoritative for gameplay; a minted `CStruct` buffer is
  authoritative for the editor and serializer. Keeping them in sync is the blob codec, the bind/apply path, the
  live-instance migration, the retire/refcount machinery, and the editor re-bind push.

The interop *layer* is not a problem. `[NativeCall]` / `[ManagedExport]` with `DisableRuntimeMarshalling` compiles
to real `calli`, and the hashed export table is a better handoff than Flax's per-method resolution. The batched
one-crossing-per-world tick beats Flax's per-script vtable-wrapper invoke. None of it is rewritten here.

---

## 3. Target architecture

### Pillar 1 — the object owns its managed instance

`CObjectBase` gains one field. There is a free 4-byte padding hole after `mutable EObjectFlags ObjectFlags`
(offset 8) and before `CClass* ClassPrivate` (offset 16) at `Core/Object/ObjectBase.h:143`, so an `int32` index
into a managed handle table costs **zero** object growth:

```cpp
private:
    mutable EObjectFlags ObjectFlags = OF_None;
    int32                ManagedHandle = INDEX_NONE;   // NEW - fits the existing padding hole
    CClass*              ClassPrivate = nullptr;
```

API on `CObject`:

```cpp
/** The managed instance for this object, created on first use. Null only if the .NET host is down. */
RUNTIME_API void* GetOrCreateManagedInstance();

/** The managed instance if one exists; does not create. */
RUNTIME_API void* GetManagedInstanceIfCreated() const;

/** Frees the handle and nulls the managed instance's Handle field. Called from BeginDestroy. */
RUNTIME_API void  ReleaseManagedInstance();
```

Handle strength rules:

- **Strong** when the managed instance carries user state — i.e. any C#-derived class. The native object's
  lifetime keeps it alive; freed in `BeginDestroy`.
- **Weak** when the managed instance is a pure wrapper over a native-owned object. It may be collected and
  transparently recreated on the next `GetOrCreateManagedInstance()`.

This is Flax's `ScriptingObject` / `ManagedScriptingObject` split, and it is the correct rule.

`ReleaseManagedInstance` **nulls the managed instance's `Handle` field** before freeing. Every generated accessor
null-checks `Handle` and throws a clean `ObjectDisposedException`-style managed exception. A stale C# reference to
a destroyed object becomes a diagnosable exception instead of memory corruption. That branch is the price of
safety on `CObject` paths and it is worth paying.

Consequences: reference identity works (`==`, `is`, pattern matching, dictionary keys); no allocation per access;
one lifetime authority (native).

### Pillar 2 — inheritance via generated shim + minted `CClass`

Keep the existing mechanism, generalize it in two ways.

**Drop the per-method opt-in.** Today the override surface is `FUNCTION(ScriptEvent)`. Emit a shim override for
every `virtual FUNCTION()` instead — users should not have to predict what someone will want to override. Class
opt-in (`REFLECT(Scriptable)`) stays.

**Move the override mask to the class.** `FScriptableBridge` currently carries `Handle`, `OverrideFlags`,
`Generation`, `Self` per instance. With Pillar 1 supplying the handle, the bridge collapses: the override mask is
a property of the *type*, so it belongs on the minted `CClass`.

```cpp
class CClass
{
    // ...
    uint64 ScriptOverrides = 0;   // bit N set = the managed subclass overrides ScriptEvent N
};
```

Generated shim dispatch becomes one load and a test of a class-level word, perfectly predicted for methods nobody
overrides:

```cpp
int32 CFoo__Script::OnTest(int32 X) override
{
    if (GetClass()->ScriptOverrides & (1ull << 3))
    {
        return DotNet::Dispatch<int32>(GetOrCreateManagedInstance(), __ScriptEvent_CFoo_OnTest, X);
    }
    return CFoo::OnTest(X);   // qualified, non-virtual
}
```

More than 64 virtuals per type needs a spill array; assert at mint time and cross that bridge if a type ever hits
it.

### Pillar 3 — C#-declared properties become real `FProperty`s on the minted class

This is the pillar that deletes the most code, and the one to prototype first.

**Mechanism.** `AllocateCObjectMemory` sizes the allocation from `Class->GetSize()`
(`Core/Object/ObjectCore.cpp:48-49`), and `CStruct::Size` is mutable at mint time (`ObjectCore.cpp:553`). So a
minted class can declare a larger size than its shim and place C#-declared properties in the trailing block:

```
+---------------------------+  offset 0
| CFoo__Script (shim)       |  ShimSize bytes  - the C++ subclass, incl. all native FPropertys
+---------------------------+  offset ShimSize
| minted property block     |  MintedSize bytes - FPropertys declared in C#
+---------------------------+
```

Mint sets `Size = Align(ShimSize, MintedAlign) + MintedSize`, calls `AddProperty` for each C#-declared
`[Property]` with `Offset >= ShimSize`, then links. `EmplaceInstance` placement-news the shim into the (larger)
block as it does today. This is the Unreal blueprint-added-property model.

**C# sees a view, not a copy.** The Roslyn generator turns a declaration into a ref-accessor over the native
block, with offsets resolved once per type at load:

```csharp
// user writes
public partial class Patrol : EntityScript
{
    [Property] public float Speed;
    [Property] public FVector3 Target;
}

// generator emits
public partial class Patrol
{
    private static int __Off_Speed;    // resolved once at type load
    private static int __Off_Target;

    public ref float    Speed  => ref Unsafe.AsRef<float>((byte*)Handle + __Off_Speed);
    public ref FVector3 Target => ref Unsafe.AsRef<FVector3>((byte*)Handle + __Off_Target);
}
```

A read is an add and a load. `Speed += dt` mutates the authoritative buffer directly. Non-blittable members
(`FString`, `TVector<T>`, `TObjectPtr<T>`, soft refs) get accessor properties that call thunks instead of a ref —
these are authoring-shaped, not hot-path.

**What this buys, all for free:**

- Editor draws script properties with the stock `FPropertyTable` — no custom drawer.
- Serialization is `SerializeTaggedProperties` — the same path scenes already use.
- Undo, prefab overrides, multi-edit, and net replication work because there is one real property list.
- An editor edit is immediately visible to a running script — no re-bind push, because there is one buffer.
- Hot-reload migration is one tagged-property round-trip over one copy, not a reconcile between two.

**Constraint accepted:** a `[Property]` member must be a type the native property system can represent. Arbitrary
managed objects can still be plain (non-`[Property]`) C# fields; they just aren't editor-exposed or serialized.
This is the same constraint `UPROPERTY` carries.

### Pillar 4 — ECS component access — MOSTLY ALREADY DONE (revised 2026-08-12)

**This pillar was written on a wrong premise and is now substantially descoped.** It originally proposed
Reflector-emitted blittable mirror structs plus `Registry.GetRef<T>` returning a `scoped ref`. Investigating
before implementing turned up two things that change the picture.

**1. The per-frame paths are already allocation-free.** Generated wrappers no longer use per-property thunks —
properties compile to `Unsafe.ReadUnaligned` at an offset resolved once per type via
`NativeBindings.PropertyOffset` — and both bulk paths reuse a single wrapper:

- `View<T...>.Each` and its `foreach` enumerator (`World/ViewTypes.cs`) allocate **one** wrapper per view via
  `ViewWrapper<T>.New()`, then rebind `W0.Handle = P[i]` per element, with the entity/pointer chunk arrays
  rented from `ArrayPool`. Iterating a million entities allocates one wrapper.
- `[RequireComponent]` fields hold one wrapper for the script's lifetime in `NativeStruct`'s *bound* mode, where
  `Handle` re-resolves the live component through the registry op-table on each access — so a stored wrapper
  cannot dangle, at the cost of a native `GetComponent` call per access instead of an allocation.

The remaining allocation is confined to ad-hoc `Registry.Get/TryGet/Emplace<T>` — 9 call sites across all C# in
the repo, none inside a per-element loop.

**2. A Reflector-emitted layout mirror would be actively unsafe.** The Reflector only sees `PROPERTY()`-marked
members, so a mirror built from them would omit unreflected fields and `ref`-ing field N would read the wrong
bytes. The current design — offsets resolved at runtime from native reflection — has no layout to drift and is
strictly safer. A `LayoutRegistry` size check catches a total-size mismatch, not a hole in the middle.

**What is left of this pillar**, if it is worth doing at all:

- `Registry.Get/TryGet/Emplace` could reuse a per-`(T, thread)` scratch wrapper the way `View` does, but that is
  only safe if the result is never stored, which a class-returning signature cannot enforce.
- Converting generated wrappers from `class` to `readonly struct` removes the allocation everywhere, but must not
  break bound-mode re-resolution or view reuse. Only 3 component wrappers use wrapper inheritance
  (`SMeshComponent` → static/skeletal/dynamic), so that part is cheap.

Neither is urgent. **Recommendation: measure a realistic script frame before spending anything here, and do
Pillar 1 first** — it removes repeated wrapper construction on the `CObject` side (`Asset.Load`,
`AssetRef.Value`, `Blackboard`), which has no equivalent reuse mechanism today.

### Pillar 5 — interop layer unchanged

`[NativeCall]`, `[ManagedExport]`, the hashed export table, `DisableRuntimeMarshalling`, and the batched
one-crossing-per-world tick all stay as they are. The per-frame dispatch shape is already better than Flax's.

### What `EntityScript` becomes

`CEntityScript : CObject`, marked `REFLECT(Scriptable)`, with lifecycle virtuals as ordinary reflected virtuals:

```cpp
REFLECT(Scriptable)
class RUNTIME_API CEntityScript : public CObject
{
    FUNCTION() virtual void OnAttach() {}
    FUNCTION() virtual void OnReady() {}
    FUNCTION() virtual void OnUpdate(float Dt) {}
    FUNCTION() virtual void OnFixedUpdate(float Dt) {}
    FUNCTION() virtual void OnDetach() {}
};
```

The ECS carrier component holds only `TObjectPtr<CEntityScript>`. **Tick dispatch still iterates the native ECS
view** — dense, cache-friendly, one batched crossing per world — it just resolves an object pointer instead of a
GCHandle. A C# script is then nothing more than "a C# subclass of a native type," which is exactly Pillar 2. No
second subsystem.

Cost accepted: one `CObject` per scripted entity (allocation + GC visitation) versus today's POD carrier. Flax
pays the same cost. If a project ever needs tens of thousands of scripted entities, the answer is an ECS system
(`[EntitySystem]`), not a per-entity script — which is already true today.

---

## 4. Safety invariants

| Hazard | Mechanism |
|---|---|
| Managed wrapper outliving its native object | `BeginDestroy` → `ReleaseManagedInstance` nulls the managed `Handle`; accessors null-check and throw |
| Stale handle across hot reload | Generation stamp checked at one choke point (a single `FManagedRef` type), not per-subsystem |
| `ref` into relocated component storage | `scoped ref` / `ref struct` — compiler-enforced, cannot be stored in a field |
| Layout drift C++ ↔ C# | `LayoutRegistry` boot validation over *all* mirrors; abort on mismatch |
| Cross-boundary GC cycles | Native ownership is authoritative; a managed instance never keeps a native object alive |
| Thread affinity | Debug-only game-thread assert in the accessor preamble |
| Minted class outliving live instances | Per-type live-instance refcount; retire, don't free, until zero (keep today's behavior) |

---

## 5. Performance

| Operation | Today | After |
|---|---|---|
| `View.Each` / `foreach` per element | already one wrapper per view, pooled chunks | unchanged (already optimal) |
| `[RequireComponent]` field access | no allocation; native re-resolve per access | unchanged |
| ad-hoc `Registry.Get<T>()` | thunk + gen0 wrapper allocation | open — see revised Pillar 4 |
| Script property read | plain managed field (fast) but a *copy* of the native buffer | add + load from the authoritative buffer |
| Script property write from editor | blob encode → crossing → reflection set → re-bind | direct buffer write, script sees it immediately |
| Script bind | offset/blob marshal of every field | nothing — the object *is* the storage |
| C#→native call | `calli` via `delegate*` | unchanged |
| Native→C# tick | one batched crossing per world | unchanged |
| Virtual dispatch into C# | per-instance flag test | per-class flag test (better predicted) |

---

## 6. What gets deleted

- Value-blob codec: `Serializer.cs` `WriteValue`/`ReadValue`/`ApplyValues`/`WriteSchema`, and the native mirrors
  `StructToValueBlob` / `ApplyPropertyValuesToStruct` / `GatherScriptSchema`.
- Minted `CStruct` schema path for scripts: `ScriptStruct.cpp` mirror minting, nested-schema serials,
  `ScriptDataStruct`, `ScriptValueBridge`, `ScriptValueStore`.
- `ScriptPropertyDrawer` and the script-specific property customizations.
- Live-instance blob migration, the retire graveyard's blob half, and the editor re-bind push.
- `Wrapper<T>` and every per-access wrapper allocation.
- Per-instance `FScriptableBridge` (reduced to the class-level override mask plus Pillar 1's handle).

Kept and repurposed: `TypeLibrary` (still needed to enumerate C# types and their `[Property]` members at mint
time), `LayoutRegistry`, `ScriptCompiler`, `ScriptLoadContext`, the ALC teardown contract.

### 6.1 Already removed (2026-08-12, net −607 lines)

Done after Phase 2, once these had no reachable consumers:

- **The Coral-style reflective interop layer.** `Scripting/DotNet/ManagedCall.h` (`FManagedValue` /
  `FManagedClass` / `FManagedObject`), its implementation in `DotNetHost.cpp` (the value-blob codec,
  `InvokeManaged`, the result sink), and the managed `ClassFind` / `ObjectNew` / `Invoke` / `FieldGet` /
  `FieldSet` entries with their private reflection helpers. Native→managed dispatch goes through generated
  `[ManagedExport]` entries and, for C# subclasses of native types, the Scriptable bridge; the C++ RAII API had
  zero callers.
  - Its one real consumer was the inspector's `[Button]`, which only ever needed "call this parameterless
    method on this instance". That is now a single purpose-built export, `ManagedCalls.InvokeScriptButton`, so
    the general machinery is gone rather than kept alive for one narrow call. `[Button]` methods are already
    contractually parameterless (`TypeLibrary.ComputeButtons` rejects anything else).
- **`PropGetString`** (C# bind + native export) — superseded by `NativeMarshal.ReadString`, which reads the
  `eastl::basic_string` in place off the property offset with no crossing. The setter is still used.
- **`GetEntityScriptHandle`** (C# bind + native export) — script-to-script resolution moved to the managed
  `EntityScriptRuntime` entity index; the crossing had no callers.

Deliberately **not** removed: `SetObjectPtr` looked unreferenced from C# but is called by
`PropSetObject` natively. Several gameplay exports (`World_GetParentEntity`, `World_GetFirstChildEntity`,
`World_GetNextSiblingEntity`, `World_GetRotation`, `World_GetScale`, `World_IsValidEntity`,
`World_SetActiveCamera`, `World_SetScale`, `GameplayTag_IsValid`, `GameplayTag_MatchesExact`) have no managed
counterpart, but they read as an API surface that was never wired up rather than something this rewrite made
obsolete — they are listed here for a deliberate decision, not deleted on suspicion.

**Needs an in-editor check:** clicking an inspector `[Button]` (the one behavior that changed rather than
simply being deleted).

---

## 7. Migration phases

Each phase builds green and ships independently. Order is chosen so value lands early and the riskiest work has
the most context behind it.

**Phase 0 — spike: appended properties. DONE — result: GO.** See §7.1.

**Phase 1 — components by ref (Pillar 4). DESCOPED — see the revised Pillar 4.** The premise (per-element
wrapper allocation on the hot path) turned out to be false: `View`/`foreach` and `[RequireComponent]` already
reuse one wrapper. What remains is ad-hoc `Registry.Get`, which is not on a per-element path. **Do not do this
phase before measuring**; Phase 2 now leads.

**Phase 2 — managed instance on `CObject` (Pillar 1). DONE.** See §7.2.

**Phase 3 — generalize the shim (Pillar 2). DONE.** See §7.3.

**Phase 4 — appended properties for real (Pillar 3).** Mint C#-declared `[Property]`s onto the minted `CClass`;
generator emits ref-accessors; editor and serializer go through the stock paths. *Verify:* `PropertyTypeTest.cs`
round-trips every supported type through save/load, undo, prefab override, and hot reload.

**Phase 5 — `CEntityScript` unification. VERTICAL SLICE DONE.** See §7.4. The unified base, component and
driver exist and are proven; porting the live C#-specific driver onto them (and deleting it) remains.

**Phase 6 — deletion.** Remove everything in §6. Do this only after Phase 5 has run through a real PIE session,
including hot reload with live instances.

---

### 7.1 Phase 0 result — GO

Run as `MintedPropertySpike.*` in `Engine/Tests/Source/MintedPropertySpikeTests.cpp`, with the minting helper in
`FScriptableRegistry::SpikeAppendFloatProperty` (`Runtime/Source/Scripting/ScriptableObject.cpp`). Three tests,
all passing; full suite 181/181 green. Both the test and the helper are marked throwaway — delete them when
Phase 4 lands the real implementation. `CScriptableTest` gained a `PROPERTY() float NativeValue` so the spike
can prove a native and an appended property coexist.

**Confirmed — every premise of Pillar 3 holds:**

- A minted `CClass` can declare `Size = Align(ShimSize, alignof(T)) + sizeof(T)` and place an `FFloatProperty`
  in the trailing block. `StaticAllocateObject` sizes both the allocation and the `Memzero` from
  `Class->GetSize()`, so every instance carries the block, zero-initialized.
- The appended property is reachable through the normal property chain, at the expected offset, past the shim.
- Instances have independent storage; the shim's vtable, its C++ constructor, and its native `PROPERTY()` member
  are all untouched (`OnTest(5) == 10`, `NativeValue == 1.5f` after writing the appended property).
- The CDO is allocated from the same enlarged size — writing the appended property on it is in-bounds.
- `CStruct::SerializeTaggedProperties` round-trips **both** the native and the appended property with no bespoke
  codec. This is the result that makes the pillar worth doing.

**Three findings that change how the real implementation must be written:**

1. **`ProcessNewlyLoadedCObjects()` does not finalize a runtime-minted class.** `AllocateStaticClass` wires
   `SuperStruct` and calls `BeginRegister`, but never enqueues into the deferred class registry — so that pass
   has nothing pending for the class, and no CDO is created. Since `CClass::CreateDefaultObject` is what calls
   `Link()`, the super's properties stay unchained until something asks for the CDO. **The mint sequence must be
   `AllocateStaticClass` → set `Size`/`Alignment` → add properties → `GetDefaultObject()`.** Note the existing
   comment in `FScriptableRegistry::RefreshMintedClasses` ("Finalize registration + create CDOs") overstates what
   that call does for minted classes; CDOs there are created lazily on first access.
2. **`CStruct::GetProperty` only walks `LinkedProperty`.** Its "Searches full inheritance chain" comment is true
   only *after* `Link()` has chained the super's list on. Any code that looks up a base-class property on a
   minted class before linking silently gets null — which is exactly how this spike failed on its first run.
3. **Property minting is Runtime-internal.** `FProperty::Init`, `FProperty::CallSetter`, `FProperty::CallGetter`
   and `FArchiveSlot::Serialize(float&)` are not `RUNTIME_API`, so constructing an `FProperty` from another
   module does not link. The registrar already lives in Runtime, so this costs nothing — but it rules out
   minting from Editor or a plugin without exporting more of the property API.

**One constraint to assert on:** `FPropertyParams::Offset` is a `uint16`, so shim + appended block must stay
under 64KB. Add a loud check at mint time rather than letting an offset silently wrap.

### 7.2 Phase 2 result — landed

Native suite 189/189 green (8 new), managed layer builds with 0 warnings, editor links.

**Shape as built** — one deviation from the sketch, and it simplifies things. The doc proposed
`CObject::GetOrCreateManagedInstance()` driving creation from native. Creation is instead driven from C#, where
the wrapper type is already known statically, so no native→managed type resolution is needed. Native owns
storage and lifetime; C# owns construction.

- `Core/Object/ManagedInstance.{h,cpp}` — `FManagedInstanceTable`: a slot array of GC handles with a free list,
  plus an owner back-reference so `ReleaseAll` can clear each object's slot index. API is
  `Find/Set/Release/ReleaseAll/GetLiveCount/GetSlotCapacity` under `ManagedInstances`, with the actual handle
  free installed by the scripting layer via `SetFreeHandleFn` (Core stays ignorant of the managed runtime).
- `CObjectBase::ManagedInstanceSlot` (`int32`) sits between `ObjectFlags` and `ClassPrivate`. **The zero-growth
  claim is confirmed by the build**, not assumed: `ManagedInstance.cpp` carries a `static_assert` comparing
  `sizeof(CObjectBase)` against a layout probe mirroring the same fields *without* the slot. A future field
  reorder that grows every CObject fails the build.
- `~CObjectBase` releases the slot, guarded on `!= INDEX_NONE` so an object that was never wrapped pays one
  compare.
- **Handles are weak.** The cache remembers the wrapper that exists; it never keeps one alive. A wrapper nothing
  references is collected normally, and the cache cannot pin the collectible script ALC.
- Managed side: `Wrapper<T>.ForObject(IntPtr)` — query the cache, return the live instance if the weak target is
  still a `T`, else create, `GCHandle.Alloc(..., Weak)`, and `Set` (which frees whatever it replaces). Wired into
  `Asset.Load`, `Asset.LoadAsync`, `TObjectPtr<T>.Value`, `Blackboard.GetObject`, and — via the Reflector — every
  generated CObject property getter.

**Hot-reload and leak validation** (`Engine/Tests/Source/ManagedInstanceTests.cpp`, 8 tests). They exercise the
native table with a counting stand-in for the GC-handle free, so the half where a leak actually lives is covered
without a running .NET host: cache-and-return, replace-frees-old-exactly-once, **destroy-frees-the-handle**,
unwrapped objects allocate no slot, **slots recycle across 64 object churns** (capacity stays ≤ 1),
**`ReleaseAll` drains every handle once and re-wrapping reuses the drained slots rather than growing** (so
repeated reloads cannot leak slots), **destroy-after-`ReleaseAll` is not a double free**, and the table works
with no free function installed. Release points: `ManagedInstances::ReleaseAll()` in `DotNetHost` right after the
generation swaps on reload, and in `Shutdown` before the managed side goes away.

**One hazard found and guarded during the emitter change.** `EBind::Object` in the Reflector covers *both*
CObject wrappers and opaque **struct** wrappers (components) — both marshal as `void*`. Routing all of them
through `ForObject` would have reinterpreted a component pointer as a `CObjectBase` and written a slot index
into component memory. Fixed with `FBinding::bCObject`, set only on the `EPropertyTypeFlags::Object` path;
verified in the regenerated output (47 files use `ForObject`, zero `Wrapper<global::Lumina.S*>` routings).

**All four wrapper-construction paths now route through the cache**, each with its own CObject/NativeStruct
discriminator:

| Path | Emitted by | Discriminator |
|---|---|---|
| Hand-written (`Asset.Load`, `TObjectPtr.Value`, `Blackboard`) | — | call site is statically a `NativeObject` |
| CObject property getters | Reflector `EmitProperties` | `FBinding::bCObject` |
| Object function returns | Roslyn `NativeCallGenerator` | `IsCObjectRef` (root is `NativeObject`) |
| ScriptEvent reverse dispatcher args | Reflector `SeArgFromAbiCS` | `FArg::bCObject` |

Verified in the regenerated output in both directions: CObject returns/args build via
`Wrapper<T>.ForObject` (`CWorld`, `CStaticMesh`, `CSkeletalMesh`, the `NativeObject` root), while component
returns still use `new` — `CWorld.GetActiveCamera()` returning `SCameraComponent` is unchanged, and no
`Wrapper<global::Lumina.S*>.ForObject` exists anywhere in generated code.

A note on the mixed-type case: asking for a *more derived* type than the cached one replaces the cache entry
(the old wrapper keeps working, it just stops being canonical); asking for a *base* type reuses the cached
derived instance, which is assignable. Both are correct, and `ForObject` documents it.

**Still open:** the managed half needs a running host — verify in editor that two `Asset.Load` calls for the
same path are now `ReferenceEquals`.

### 7.3 Phase 3 result — landed

192/192 native tests green (3 new), managed layer 0 warnings.

**`FScriptableBridge` is gone.** The shim now carries no per-instance state at all — no member, no
`PostInitProperties` override, no destructor. It is nothing but the overrides:

```cpp
virtual void PreWorldLoad(Lumina::CWorld* A0) override
{
    if (GetClass()->ScriptOverrides & (1ull << 0))
    {
        void* __h = Lumina::Scriptable::GetOrCreateInstance(this);
        ...
        if (__h && __t) { __t(__h, (void*)A0); return; }
    }
    Lumina::CGameInstance::PreWorldLoad(A0);
}
```

- **Override mask on the class.** `CClass::ScriptOverrides`, stamped at mint from the managed enumeration (the
  mask was always type-uniform — `ScriptableRuntime` already cached it per type and then shipped it back per
  instance). Dispatch is now one load of a word that is zero for every native class. It is re-stamped on every
  refresh, not just first mint, because minted classes are reused by name across reloads.
- **Managed instance from the Pillar 1 slot.** `Scriptable::GetOrCreateInstance` looks in the object's slot,
  creates on miss, and stores. That deleted the per-instance `Handle`/`Generation`/`Self`, `DestroyScriptable`,
  and `ScriptableRuntime`'s `LiveHandles`/`Destroy`/`FreeAll`.
- **The generation gate disappeared with it.** A hot reload drains the table, so "the slot is empty" *is* the
  rebind condition — there is no longer a per-instance generation stamp to keep in step with the host.
- **Handle ownership is single-sourced.** Scriptable handles are strong (they hold user state), so they would
  pin the collectible ALC; they are released at exactly the same point in the teardown contract as before,
  `ScriptManager.UnloadCurrent`, which now calls `Native.ReleaseAllManagedInstances()` instead of keeping a
  second list. One owner, one release point.

**Dropping the per-method opt-in.** `IsScriptEvent` now gates on `Fn.bIsVirtual && Type.HasMetadata("Scriptable")`
rather than a `FUNCTION(ScriptEvent)` marker: the author marks the *class*, and any reflected virtual on it is
overridable. Two things made this safe to do:

- `bIsVirtual` was recorded by the visitor but consumed nowhere, and a note claimed `clang_CXXMethod_isVirtual`
  returned 0 for inline-defined virtuals. A probe emitted into the generated output disproved that — all four
  events on the two Scriptable classes report `bIsVirtual=1`, including the inline `CScriptableTest::OnTest`.
- The second gate (`Scriptable`) is load-bearing. `IsScriptEvent` is also what makes `EmitFunctions` skip a
  method; without it, a virtual on an ordinary reflected class would be skipped there and emitted nowhere,
  silently dropping it from the C# API.

The change is **behavior-neutral today** — both Scriptable classes still emit exactly the same two events,
because every virtual on them was already marked. Its value is forward-looking: new virtuals need no marker.
The legacy `FUNCTION(ScriptEvent)` markers are now redundant and harmless.

**New tests** (`ScriptableTests.cpp`): the mask is stamped on the minted class and empty on native classes;
with the bit set but no managed instance obtainable, dispatch falls back to the C++ default, stays stable
across repeated calls, and occupies no slot (a failed create must not leak one per object); the CDO never
binds an instance.

**Needs an in-editor check:** `SandboxGameInstance`'s hooks firing, and a script hot reload with a live
Scriptable instance.

### 7.4 Phase 5 result — the unification, proven

204/204 native tests green (5 new). This is the slice the whole rewrite was aimed at.

`Scripting/EntityScript.{h,cpp}`:

- **`CEntityScript : CObject`, `REFLECT(Scriptable)`** with `FUNCTION() virtual` `OnAttach` / `OnReady` /
  `OnUpdate(float)` / `OnFixedUpdate(float)` / `OnDetach`, plus its owning entity.
- **`SEntityScriptComponent`** holding `TVector<TObjectPtr<CEntityScript>>` — language-agnostic, because every
  element is just a `CEntityScript` of *some* `CClass`.
- **`EntityScripts::Attach / Tick / TickFixed / DetachAll`** — the entire driver, and the reason this matters:

```cpp
for (auto&& [Entity, Component] : Registry.view<SEntityScriptComponent>().each())
    for (TObjectPtr<CEntityScript>& Held : Component.Scripts)
    {
        if (!Script->IsReady()) { Script->MarkReady(); Script->OnReady(); }
        Script->OnUpdate(DeltaTime);
    }
```

One loop of plain virtual calls, with **no language-specific code anywhere in it**. A C++ subclass runs its own
override directly; a C# subclass runs the Reflector-generated shim, which dispatches into managed. The driver
cannot tell the difference and gains nothing when a third language is added. Compare the system it replaces:
334 lines of C#-specific binding, callback masks, generation gates and blob application.

**What Phase 3 bought here, visibly.** `CEntityScript`'s five virtuals carry **no `ScriptEvent` markers** — the
class is marked, and the Reflector did the rest: it emitted a shim gating each on
`GetClass()->ScriptOverrides & (1ull << N)`, and a C# wrapper with `[ScriptEvent(0..4)]` virtuals ready to be
overridden. Writing the base was the whole cost of making it scriptable from C#.

**The unification test** attaches a *minted* `CClass` deriving `CEntityScript` — the exact shape the host
creates for a C# subclass — and a C++ script **to the same entity**, then ticks both through the one loop and
asserts both readied and ran. Also covered: lifecycle order (attach at attach, ready deferred to first tick and
only once), delta-time pass-through, fixed update refusing to run before ready, many scripts per entity and
many entities, and non-`CEntityScript` classes being refused rather than attached.

**What this unlocks:** C++ entity scripts, for free. Subclass `CEntityScript`, override what you need, attach
it — no C# involvement, no separate system.

**Still to do before the old path can go:** port the live driver's remaining behavior (input dispatch,
collision/perception callbacks, the PrePhysics/PostPhysics split, `[Property]` values) onto this base, register
a real ECS system that calls `Tick`/`TickFixed`, move the C# `EntityScript` class to derive the generated
`CEntityScript` wrapper, then delete `SScriptComponent`/`SScriptInstance` and `CSharpScriptSystem`. The two
paths coexist today; nothing was removed.

### 7.5 The cutover (steps 1-3) — executable spec

**Decision (2026-08-12): full conversion, no fallback, no legacy.** Breaking existing script syntax is
approved, so `[Property] public partial float Speed { get; set; }` over native storage is the target.

**Enabler landed:** `CEntityScript` now has `FUNCTION() entt::entity GetOwningEntity()` and
`FUNCTION() CWorld* GetWorld()` (world resolved at attach from `Registry.ctx().find<CWorld*>()` — `find`, so a
bare test registry simply has none). The generated wrapper exposes both to C#.

**These three steps must land together.** Step 1 alone breaks the running engine: an `Activator`-created script
would have an *unbound* `NativeObject` handle, so every accessor throws. Do not stage them.

**Step 1 — `Engine/Source/LuminaSharp/Scripting/EntityScript.cs`**
- `public abstract class EntityScript : Lumina.CEntityScript`.
- Delete `OnAttach` / `OnReady` / `OnUpdate` / `OnFixedUpdate` / `OnDetach` — inherited from the wrapper as
  `public virtual` with `[ScriptEvent(0..4)]`.
- `Entity` ⇒ `public Entity Entity => GetOwningEntity();`, `World` ⇒ `public Lumina.CWorld World => GetWorld();`
  (delete the `internal set`s and the `WorldHandle` / `SelfHandle` fields — the native object owns both now).
- `OnInput` stays managed-declared but is **inert until step 5** wires input dispatch. Mark it as such; do not
  leave it looking live.
- `CachedTransform` can stay (a component view, unrelated to this).

**Step 2 — creation path**
- Scripts are created by `EntityScripts::Attach(Registry, Entity, MintedClass)`; the managed instance follows
  from the shim's first dispatch via `Scriptable::GetOrCreateInstance` → `ScriptableRuntime.Create`. That path
  already exists and is tested — nothing new is needed on the managed side.
- Delete `EntityScriptRuntime`'s `Create` / `Destroy` / `FreeAll` / `ByEntity` index and the `CreateEntityScript`
  / `DestroyEntityScript` / `UpdateScripts` / `FixedUpdateScripts` host exports with their native typedefs.
- New native exports for the script-lookup API the C# base exposes, over `SEntityScriptComponent`:
  `AttachEntityScript(world, entity, className)`, `FindEntityScript(world, entity, className)`,
  `FindEntityScripts(...)`, `RemoveEntityScript(world, entity, script)` — each returning/taking a
  `CEntityScript*`, wrapped in C# by `Wrapper<T>.ForObject` (identity holds, Phase 2).
- Re-base `EntityRegistry.GetScript<T>` / `GetScripts<T>` / `AddScript<T>` / `RemoveScript<T>` onto those.

**Step 3 — component, serialization, picker**
- Replace `SScriptComponent` / `SScriptInstance` with `SEntityScriptComponent`.
- **Serialization is the one genuinely new piece.** Scripts are per-entity *subobjects*, so a `TObjectPtr`
  property would path-serialize and not round-trip. Give `SEntityScriptComponent` a custom struct serializer
  (`FStructOps::HasSerializer`) that writes, per script: the class name, then
  `Script->GetClass()->SerializeTaggedProperties(Ar, Script)`; and on read does `FindObject<CClass>(name)` →
  `EntityScripts::Attach` → deserialize into the new instance. That reuses the stock tagged serializer for the
  values (Phase 4) and needs no `SScriptValueStore`.
- Editor picker: `CSharpScriptComponentCustomization` currently edits `SScriptInstance::ScriptClass` (a
  string). Re-point it at the class list filtered to `CEntityScript` subclasses — which now includes **C++
  scripts**, for free.

*Verify:* the existing 5 `EntityScriptUnification` tests still pass; a scene with a C# script round-trips
save/load; PIE runs a C# script's `OnUpdate`; hot reload with a live script instance.

### 7.6 Property completeness — every reflected kind, both sides (2026-08-12)

**Landed.** The class-append path no longer supports a subset of kinds; it supports whatever the property
types themselves implement, because the layout is planned by the *same* code that plans a `CScriptStruct` and
value lifetime is the property's own business.

**The blocker first.** Appending an `FString` used to take an access violation at process teardown. Cause, now
fixed: `~CObjectBase` reaches through `ClassPrivate` to tear down the appended block, and
`FCObjectArray::Shutdown` walked objects in index order, which freed a minted class before its own CDO. Index
order is not creation order (freed indices are recycled), so no ordering of the single pass could have been
correct. Two changes, each independently worth having:

- `FCObjectArray::Shutdown` destroys in two passes, instances before the `CField`s that describe them, so a
  class always outlives its instances. That invariant was already assumed elsewhere (`TryRetireMintedClass`
  refuses to retire a class with live instances); shutdown just did not honor it.
- `OF_ScriptProperties` marks the objects that actually have trailing script storage. Only those reach through
  their class in the destructor, so no native object's teardown depends on class lifetime at all.

**The generalization.** `FProperty` gained `ConstructValue` / `DestructValue` / `OwnsStorage`, overridden by
`FStringProperty`, `FSoftObjectProperty`, `FObjectProperty` (release the strong ref), `FInstancedStructProperty`,
`FStructProperty` (defers to `CStruct::RequiresValueLifecycle`), `FDelegateProperty`, and
`FArrayProperty`/`FMapProperty` (through new `Construct/DestructContainer` + `Context` entries on
`FVectorOps`/`FMapOps`, appended after `ElementSize` so the `LuminaSharp.VectorOps` ABI holds). Nothing else
switches on a kind:

- `CClass::Construct/DestructScriptProperties` walk `ScriptLifecycleProperties`.
- `CScriptStruct::ConstructInto`/`DestructIn` walk the property list. `FFieldInfo`, `EScriptElementKind` and
  their two parallel lifecycle switches are gone.
- `FScriptArrayElementDesc::Construct/DestructElement` defer to the element's inner property.

`CScriptStruct::EmitLayoutInto(Target, BaseOffset, Schema)` is the one planner. A minted class gets a
transient `CScriptStruct` as its **layout record** — it owns the element descriptions, minted sub-structs and
minted enums the appended properties point at, and outlives the class's instances.

**Defaults.** A minted class has no C++ constructor, so declared defaults are written once to the CDO and
every instance is copied from it inside `ConstructScriptProperties`, via `CopyCompleteValue` (correct for
storage-owning kinds, not just memcpy-able ones).

**C# side.** `ScriptPropertyGenerator` used to emit `Unsafe.ReadUnaligned<T>` for every type, which for a
`string` reinterprets native bytes as a managed object reference. It now classifies by C# type and emits the
matching access — in place for unmanaged and blittable struct mirrors, through a `long` for enums (the minted
slot is int64 whatever the C# underlying type is), `ReadString`/`PropSetString` for `string`, path get/set for
`FSoftObjectPath`, canonical wrapper for object references, and a `NativeList<T>` view for containers — and
reports **LUM0101** (unsupported type) or **LUM0102** (container declared with a setter) rather than emitting
something that cannot marshal.

Two ordering hazards fixed with it: offsets/tokens resolve lazily (`LazyPropertyOffset`/`LazyPropertyToken`)
rather than in a static initializer that can fire before the class is complete, and every accessor is gated on
`NativeObject.HasNativeStorage` because the schema pass Activator-creates one *unbound* instance per script
type to describe it.

**Coverage.** `MintedPropertySpikeTests` now mints a class carrying one field of every kind (scalar, bool,
enum, string, name, soft object, object, native struct, script sub-struct, string array, int array, map,
instanced struct) and asserts placement past the shim, per-instance storage, container usability through the
stock `FArrayProperty`/`FMapProperty` API, and tagged-serializer round-trip. 211/211 green.
`Engine/Resources/Scripts/ScriptPropertyTypeTest.cs` is the end-to-end counterpart: attach it to an entity and
it writes/reads/logs every supported type.

**Still open:** hot-reload *layout migration*. `AppendScriptPropertiesToClass` runs only on a first mint, so a
reload that changes a type's property set leaves the class on its old layout until the editor restarts. Safe
(no corruption, no stale reads) but incomplete; the layout record kept per class in `ScriptStruct.cpp` is the
hook for it.

### 7.7 The `partial` is gone — a rewriter, not a generator (2026-08-12)

A script property is now an ordinary field:

```csharp
[Property] public float Speed = 5.0f;
```

No `partial` on the member, none on the class, and the initializer is the default.

**What changed is the tool, not the model.** The value still lives in native memory, for the reasons in
Pillar 3. What forced `partial` was purely that a *source generator* may only ADD members, never replace one,
so the author had to declare something for it to fill in — and a partial property may not carry an
initializer, which is why defaults had nowhere to live.

That constraint never applied here, because the engine compiles scripts itself. `ScriptCompiler` owns the
syntax trees, and its output is the only assembly ever loaded: the generated `.csproj` is IntelliSense-only,
and `ProjectPackager` stages `FScriptUnit::AssemblyPath` — this compiler's emit. So
`ScriptPropertyRewriter` transforms the trees before `CSharpCompilation.Create` and turns each `[Property]`
field into the native-backed property it has to be. A probe compilation is built first solely to give the
rewriter a `SemanticModel`; a rewrite error aborts the compile naming file and line.

**Defaults fall out.** The initializer is lifted into a generated `__ApplyScriptDefaults`, which the engine
runs once against the CDO through a temporary managed instance bound to it (`ScriptableRuntime.ApplyDefaults`).
`CClass::ConstructScriptProperties` already copies every instance from the CDO. It is deliberately not a
constructor: the managed wrapper is created *after* a loaded object holds its authored values, so assigning
there would overwrite them.

`ScriptPropertyGenerator` no longer emits anything — it is validation only (LUM0101 unsupported type, LUM0102
container initialized, LUM0103 partial property), because the rewriter runs at load time and without an
analyzer a bad member would look fine in the IDE and fail only on reload.

**Four traps, each of which compiled cleanly while doing nothing:**

1. `ParseMemberDeclaration` returns only the FIRST member of a text — it dropped the property and kept the
   offset static. Parse a wrapper class and take its members.
2. Overriding a `protected internal` member from another assembly must be declared `protected`.
3. **The `[Property]` attribute has to be copied onto the emitted property.** `TypeLibrary` discovers members
   by that attribute at run time; without it everything compiles and nothing is published.
4. `#line` must be attached as trivia, not written into the parsed text, or it binds to the wrapper class's
   brace and vanishes.

Three of those are invisible to a compile check, which is why the rewriter is exercised by a harness that
runs the real thing (probe-compile → rewrite → recompile → emit) rather than by inspecting generated text.

## 8. Risks and open questions

- ~~Appended-property allocation is the load-bearing unknown.~~ **Resolved by Phase 0 (§7.1).** The remaining
  unknown in this area is narrower: non-trivial appended properties (`FString`, `TVector<T>`, soft refs) need
  construct/destruct/copy over the trailing block, which the spike did not exercise — it used a trivial `float`.
  `CScriptStruct`'s existing chain-recursive `ConstructInto`/`DestroyStruct` walkers are the model to follow.
- **Ref-returning accessors and `Unsafe.AsRef` are unverifiable IL.** Acceptable inside the engine assembly;
  worth confirming no target platform enforces verification.
- **Hot reload with live instances still needs a migration step.** It shrinks to one tagged-property round-trip
  over one buffer, but it does not disappear.
- **`CObject` per script instance** costs more than the current POD carrier. Measure with a few thousand scripted
  entities before Phase 5 lands.
- **`ScriptOverrides` as a `uint64`** caps a type at 64 script-visible virtuals. Assert at mint; spill array only
  if something real hits it.
- **`ManagedHandle` in the padding hole** assumes the current field order and `EObjectFlags` being 4 bytes. Add a
  `static_assert` on `sizeof(CObjectBase)` so a future field reorder doesn't silently grow every object.

## 9. Rejected alternatives

- **Flax-style vtable swapping** — universal coverage, but UB and per-architecture machine-code parsing. The
  generated shim gets the same capability in standard C++.
- **Making the editor property grid managed** — would let us adopt Flax's data model wholesale, but it means
  rewriting the editor, which is far larger than this project.
- **Abstracting `FPropertyTable` over a virtual data source** so it could draw managed-authoritative data
  directly. Plausible, and strictly less work than a managed editor, but it leaves script data in two places and
  keeps the sync problem alive. Pillar 3 removes the second copy instead.
- **Keeping the current blob bridge and only optimizing it** — the codec is not the cost; the second copy is.
