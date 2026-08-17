# Struct-Backed Blackboards

Scope for replacing the blackboard's name-keyed value map with a strongly typed reflected struct.

Status: scope only, nothing implemented.

## The problem

A blackboard schema can already be declared as an ordinary reflected struct: `CBlackboard::BackingStructName`
resolves one, and `SyncKeysFromBackingStruct` derives the key list from its properties. The editor even
tells you to write `struct SMyBlackboardData : public SBlackboardDataBase`.

But the live values do not live in that struct. They live in `THashMap<FName, FBlackboardValue>` on
`SBlackboardComponent`. So you declare the data strongly typed and then read and write it weakly typed:

```cpp
Blackboard->SetFloat("Speed", Value);   // typo compiles, silently writes a scratch key
```

That is the worst of both arrangements. It carries the ceremony of a schema and gives none of the safety.
`FBlackboardValue` also pays for every type at once (scalar, vector, object, entity, name, string) on every
key regardless of what the key actually holds.

## The end state

```cpp
REFLECT()
struct SLocomotionData : SBlackboardDataBase
{
    GENERATED_BODY()
    PROPERTY() float Speed = 0.0f;
    PROPERTY() bool  bGrounded = false;
    PROPERTY() TObjectPtr<CAnimation> CurrentAttack;
};
```

```cpp
Comp->GetData<SLocomotionData>()->Speed = Value;   // a typo is a compile error
```

The animation graph still binds by name at author time, because the graph asset has to reference something
before the entity exists. The difference is that the name resolves to a byte offset once at link, and the
per-frame pull becomes a flat read from one base pointer.

## What already exists

This is why the migration is smaller than it looks.

- `FInstancedStruct` (`Core/Object/InstancedStruct.h`): `InitializeAs(CStruct*)`, `GetMemory()`,
  `GetMutableMemory()`, and a type-checked `Get<T>()`. This is exactly "a type plus an instance".
- `SBlackboardDataBase` marker, and the C# `[BlackboardData]` attribute that maps to it.
- `CBlackboard::BackingStructName` + `FDataStructResolveCache` + `SyncKeysFromBackingStruct`.
- `FScriptDataStructRegistry::GetGeneration()`, already used by the resolve cache to notice re-minting.
- The name-to-offset resolution and grouped-fetch loop written and then removed earlier (recoverable from
  git). Pointed at one struct instead of arbitrary components, it gets simpler: no sparse-set probe at all.
- `DotNetProperty.cpp` + `LazyProperty.cs`: C# already reads and writes blittable fields directly at a
  resolved offset. The typed C# surface is this mechanism, not a new one.

## What gets deleted

Worth stating, because it is most of the current surface area:

- `FBlackboardValue` and the hash map.
- `EBlackboardKeyType`. A struct's property types are the types.
- Every per-type default on `FBlackboardKey` (`DefaultFloat`, `DefaultInt`, `DefaultBool`, `DefaultVector`,
  `DefaultObject`, `DefaultName`, `DefaultString`). A struct's member initializers are the defaults.
- `MakeDefaultValue`, `MapPropertyToKeyType`, `ReadDefaultFromProperty`, `KeysMatch`.
- `EnsureInitialized`'s reconcile walk and the `SchemaRevision` counter added for it.

## Phases

### 1. Storage swap

`SBlackboardComponent` holds `FInstancedStruct Data` in place of `Values` / `SeededSchema` /
`SeededRevision`. Re-initialize when the resolved `CStruct` or the script-registry generation changes.
Add `GetData<T>()` / `GetMutableData<T>()`.

### 2. Name API becomes a shim

`SetFloat(Name, v)` resolves `Struct->GetProperty(Name)` and writes at the offset. Slower per call than the
hash map, which is correct: it is now the legacy path and typed access is the fast one. Keeps the ~30 C++
call sites and the whole C# surface working while the rest lands.

### 3. Animation graph binds by offset

Graph authoring is unchanged (parameters are still names). Link resolves each name against the blackboard's
struct into `{offset, type}`. `SAnimationSystem` then does one `Data.GetMemory()` plus N offset reads per
entity, replacing the current per-parameter `GetFloat` call. Object parameters take the identical path.

### 4. Typed C# surface

`[BlackboardData]` structs get a generated accessor so `bb.Data.Speed = 5f` writes native memory at the
resolved offset, reusing the existing blittable property path. Deprecate `Blackboard.SetFloat("Speed", v)`.

### 5. Retire the hand-authored key list

Either `CBlackboard` becomes a thin asset that names a struct and carries per-key metadata, or it disappears
and the graph references a `CStruct` name directly. See the open decision below.

## Risks and decisions

**Hot reload layout migration is the real hazard.** C# script structs are re-minted on reload. A hash map
keyed by name survives a field reorder; a raw struct blob does not. On a generation change the component
must re-mint into a fresh instance and copy across by property name, not by memcpy. Script property layout
migration is already a known open item, so this makes an existing gap load-bearing rather than creating a
new one. Nothing else in this plan is hard; this part is.

**Does `CBlackboard` survive?** Keeping it as a thin asset preserves the asset picker, and gives per-key
metadata (`ReadOnly`, `Hidden`, `ObjectClass`, `EnumType`) somewhere to live. Deleting it is the cleaner end
state, and that metadata arguably belongs in `PROPERTY(...)` tags on the struct instead. Recommend keeping
it through phase 4 and deciding once it is clear whether the metadata is used.

**Runtime scratch keys stop working.** Today `SetFloat("anything", v)` silently creates a value that lives
as long as the component. Against a struct, an undeclared write has nowhere to go and must fail loudly.
That is the point of the change, but the ~30 call sites need an audit for anything relying on it.

**Per-key metadata moves.** The pickers currently read `FBlackboardKey::Flags`; they would read property
metadata instead.

## Effort

Phases 1 to 3 are the substance and are mostly mechanical, since the offset machinery and the instanced
struct both exist. Phase 4 is small if the C# blittable path generalizes as expected. Phase 5 is a
judgement call more than work. The hot reload migration in phase 1 is the only part with real design risk
and should be prototyped first, because if it cannot be made reliable the whole plan is worse than what is
there today.
