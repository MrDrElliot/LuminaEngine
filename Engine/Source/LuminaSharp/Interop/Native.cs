using System;

namespace LuminaSharp;

// The reflection primitives native exports to managed code, each generated from its [NativeCall] declaration by NativeCallGenerator.
public static unsafe partial class Native
{
    private static FExporterTable Exports;

    internal static void SetExports(FExporterTable Table)
    {
        Exports = Table;
    }

    // The engine's own threshold read in place, so a filtered message is never encoded or sent.
    public static bool IsLogEnabled(ELogLevel Level)
    {
        return Exports.Log != null && (byte)Level >= System.Threading.Volatile.Read(ref *Exports.LogThreshold);
    }

    // The profiler's own flag read in place, so a script sample nothing records costs no crossing.
    public static bool IsProfilingScopes => Exports.ScriptScopesActive != null && System.Threading.Volatile.Read(ref *Exports.ScriptScopesActive) != 0;

    // Through the bootstrap exporter table, captured in the default context so it outlives every script reload.
    public static void Log(ELogLevel Level, string Message)
    {
        if (!IsLogEnabled(Level))
        {
            return;
        }

        Span<byte> Scratch = stackalloc byte[1024];
        Interop.FInteropString Encoded = new(Message, Scratch);
        try
        {
            Exports.Log((int)Level, Encoded.Pointer, Encoded.Length);
        }
        finally
        {
            Encoded.Free();
        }
    }

    public static void Log(ELogLevel Level, ReadOnlySpan<byte> Utf8)
    {
        if (!IsLogEnabled(Level))
        {
            return;
        }

        fixed (byte* Text = Utf8)
        {
            Exports.Log((int)Level, Text, Utf8.Length);
        }
    }

    // TSubclassOf. The setter is a shim rather than a byte write so the MetaClass constraint is enforced.
    [NativeCall] public static partial IntPtr PropGetClass(IntPtr C, IntPtr Prop);
    [NativeCall] public static partial void PropSetClass(IntPtr C, IntPtr Prop, IntPtr Class);
    [NativeCall] public static partial IntPtr FindClassByName(string Name);
    [NativeCall] public static partial IntPtr NewObject(IntPtr Class, IntPtr Package, string Name);

    // Reflected functions, for the dispatcher that reads a call frame through its parameters.
    [NativeCall] public static partial int FunctionParamCount(IntPtr Function);
    [NativeCall] public static partial IntPtr FunctionParamAt(IntPtr Function, int Index);
    [NativeCall] public static partial IntPtr FunctionReturnParam(IntPtr Function);
    [NativeCall] public static partial string FunctionGetName(IntPtr Function);

    // Points native dispatch at the generated invoker, and drops every such pointer before a reload.
    [NativeCall] public static partial void FunctionPublishInvoker(IntPtr Function, nint Invoker, IntPtr Offsets);
    [NativeCall] public static partial void FunctionClearInvokers();

    // A property's offset in its container, which addresses a call frame's slots without a crossing per argument.
    [NativeCall] public static partial int PropertyOffset(IntPtr Prop);

    // The width of the slot at that offset, which for an enum is its underlying type's width.
    [NativeCall] public static partial int PropertySize(IntPtr Prop);
    [NativeCall] public static partial string ClassGetName(IntPtr Class);
    [NativeCall] public static partial IntPtr ClassGetDefaultObject(IntPtr Class);

    // Copies a whole struct value into the property's slot, for a reflected function returning one by value.
    [NativeCall] public static partial void PropCopyStruct(IntPtr C, IntPtr Prop, IntPtr Source);

    // TSubStructOf. The setter is a shim rather than a byte write so the MetaStruct constraint is enforced.
    [NativeCall] public static partial IntPtr PropGetSubStruct(IntPtr C, IntPtr Prop);
    [NativeCall] public static partial void PropSetSubStruct(IntPtr C, IntPtr Prop, IntPtr Struct);
    [NativeCall] public static partial IntPtr FindStructByName(string Name);
    [NativeCall] public static partial string StructGetName(IntPtr Struct);

    // Instanced structs, addressed by raw pointer so a member and a container element use the same calls.
    [NativeCall] public static partial string InstancedStructGetType(IntPtr Ptr);
    [NativeCall] public static partial IntPtr InstancedStructGetMemory(IntPtr Ptr);
    [NativeCall] public static partial int InstancedStructIsA(IntPtr Ptr, string TypeName);
    [NativeCall] public static partial void InstancedStructInitializeAs(IntPtr Ptr, string TypeName);
    [NativeCall] public static partial void InstancedStructReset(IntPtr Ptr);

    // An FSoftObjectPath at a raw address, a member or a container element alike.
    [NativeCall] public static partial string SoftPathGet(IntPtr PathPtr);
    [NativeCall] public static partial void SoftPathSet(IntPtr PathPtr, string Path);

    // Copies one reflected struct value over another, for a container element whose type owns heap members.
    [NativeCall] public static partial void StructAssign(IntPtr Dst, IntPtr Src, string TypeName);

    // TOptional members, the get returning the payload address or zero when unset.
    [NativeCall] public static partial IntPtr PropOptionalGetValue(IntPtr Container, IntPtr Prop);
    [NativeCall] public static partial IntPtr PropOptionalInner(IntPtr Prop);
    [NativeCall] public static partial void PropOptionalSetValue(IntPtr Container, IntPtr Prop, IntPtr Value);
    [NativeCall] public static partial void PropOptionalReset(IntPtr Container, IntPtr Prop);

    // TOptional at a raw address, for a call frame slot.
    [NativeCall] public static partial IntPtr OptionalValueAt(IntPtr Member, IntPtr Prop);
    [NativeCall] public static partial void OptionalSetValueAt(IntPtr Member, IntPtr Prop, IntPtr Value);
    [NativeCall] public static partial void OptionalResetAt(IntPtr Member, IntPtr Prop);

    // A component type's ops token, resolved once per type and handed to every component call after.
    [NativeCall] public static partial IntPtr FindComponentOps(string TypeName);

    // Pure reads with no managed re-entry, so they skip the GC transition.
    [NativeCall(SuppressGCTransition = true)] public static partial IntPtr GetComponent(ulong World, uint Entity, IntPtr Ops);
    [NativeCall(SuppressGCTransition = true)] public static partial int HasComponent(ulong World, uint Entity, IntPtr Ops);
    [NativeCall(SuppressGCTransition = true)] public static partial IntPtr GetComponentRead(ulong World, uint Entity, IntPtr Ops);
    [NativeCall(SuppressGCTransition = true)] public static partial IntPtr GetComponentTracked(ulong World, uint Entity, IntPtr Ops, uint** OutEpoch);

    [NativeCall(EntryPoint = "LuminaSharp_TrackComponentPointer")]
    private static partial int TrackComponentPointerRaw(IntPtr Payload, IntPtr Ops, ulong* OutWorld, uint* OutEntity, uint** OutEpoch);

    public static bool TrackComponentPointer(IntPtr Payload, IntPtr Ops, out ulong World, out uint Entity, out uint* Epoch)
    {
        ulong OutWorld = 0;
        uint OutEntity = 0;
        uint* OutEpoch = null;
        bool bFound = TrackComponentPointerRaw(Payload, Ops, &OutWorld, &OutEntity, &OutEpoch) != 0;
        World = OutWorld;
        Entity = OutEntity;
        Epoch = OutEpoch;
        return bFound;
    }

    // These fire construct and destroy signals, which can re-enter managed code, so they keep the GC transition.
    [NativeCall] public static partial IntPtr EmplaceComponent(ulong World, uint Entity, IntPtr Ops);
    [NativeCall] public static partial int RemoveComponent(ulong World, uint Entity, IntPtr Ops);

    // Registry signals, Kind being construct, destroy or update, with Patch firing update by hand.
    [NativeCall] public static partial IntPtr RegistryConnect(ulong World, IntPtr Ops, int Kind);
    [NativeCall] public static partial IntPtr RegistryGetSignalDelegate(IntPtr Listener);
    [NativeCall] public static partial void RegistryDisconnect(ulong World, IntPtr Ops, int Kind, IntPtr Handle);
    [NativeCall] public static partial void RegistryPatch(ulong World, uint Entity, IntPtr Ops);

    // View iteration gathers a chunk of entities and their component pointers per crossing, see DotNetView.cpp.
    [NativeCall] public static partial IntPtr ViewBegin(ulong World, IntPtr* IncludeOps, int NInc, IntPtr* ExcludeOps, int NExc);
    [NativeCall] public static partial IntPtr QueryViewBegin(ulong World, IntPtr* IncludeOps, int NInc, uint WriteMask, IntPtr* ExcludeOps, int NExc, IntPtr* RequireOps, int NReq, IntPtr* FilterOps, int NFilter, uint AddedMask, uint Since, uint* OutStampTick);
    [NativeCall(SuppressGCTransition = true)] public static partial uint AdvanceChangeTick();
    [NativeCall] public static partial int ViewNextChunk(IntPtr State, uint* OutEntities, IntPtr* OutPtrs, uint** OutTicks, int MaxCount, int NInclude);
    [NativeCall] public static partial void ViewIncludeEpochs(IntPtr State, uint** OutSlots, int NInclude);
    [NativeCall] public static partial void ViewEnd(IntPtr State);

    // The strong reference a canonical wrapper owns.
    [NativeCall] public static partial IntPtr PinObject(IntPtr Object);
    [NativeCall] public static partial void UnpinObject(IntPtr Pin);

    // An object's array slot and generation, which NativeObject checks to tell a live object from a reused slot.
    [NativeCall(SuppressGCTransition = true)] public static partial long ObjectGetHandle(IntPtr Object);
    [NativeCall(SuppressGCTransition = true)] public static partial IntPtr ObjectResolve(int Index, int Generation);
    [NativeCall(SuppressGCTransition = true)] public static partial IntPtr ObjectGetEntry(IntPtr Object);
    [NativeCall(SuppressGCTransition = true)] public static partial int ObjectLayoutOffset(int Which);

    // The managed instance each object keeps, see Core/Object/ManagedInstance.h. The table locks and frees GC handles, so no suppressed transition.
    [NativeCall] public static partial IntPtr ObjectGetManagedInstance(IntPtr Object);
    [NativeCall] public static partial void ObjectSetManagedInstance(IntPtr Object, IntPtr Handle);
    [NativeCall] public static partial int ObjectAdoptScriptTwin(IntPtr Object, IntPtr WeakHandle);

    // Frees every managed instance before the collectible context unloads, since a subclass instance's strong handle would pin it.
    [NativeCall] public static partial void ReleaseAllManagedInstances();

    // The element ops of an array or map property, so a view can be built over any reflected container.
    [NativeCall] public static partial IntPtr PropVectorOps(IntPtr Prop);
    [NativeCall] public static partial IntPtr PropMapOps(IntPtr Prop);

    // Writes an FString at a raw address, since reads decode the native string in place with no crossing.
    [NativeCall(SuppressGCTransition = true)] public static partial void StringAssign(IntPtr String, string Value);

    // A TStrongObjectPtr at a raw address, the write keeping the reference count a raw pointer store would skip.
    [NativeCall] public static partial void SetObjectPtr(IntPtr Slot, IntPtr Object);
    [NativeCall] public static partial IntPtr GetObjectPtr(IntPtr Slot);

    // FName interning by address, so one pair serves a member and a container element.
    [NativeCall] public static partial void NameFromString(string Value, IntPtr OutName);
    [NativeCall] public static partial string NameToString(IntPtr Name);

    // Delegate properties, bound to a managed closure and described by their reflected arguments.
    [NativeCall] public static partial ulong DelegateBind(IntPtr Delegate, ulong Callback);
    [NativeCall] public static partial void DelegateUnbind(IntPtr Delegate, ulong Handle);
    [NativeCall] public static partial int DelegateManagedListeners(IntPtr Delegate, ulong* OutIds, ulong* OutTokens, int Capacity);
    [NativeCall] public static partial int DelegateArgCount(IntPtr DelegateProperty);
    [NativeCall] public static partial IntPtr DelegateArgProperty(IntPtr DelegateProperty, int Index);
}
