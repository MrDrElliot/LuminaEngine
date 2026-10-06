using System;

namespace LuminaSharp;

// The networking exports in DotNetGameplay.cpp. Booleans cross as int, since every one of them is a yes or no answer.
internal static unsafe partial class NetNative
{
    [NativeCall(EntryPoint = "LuminaSharp_Net_GetLocalConnection")] public static partial uint GetLocalConnection(ulong World);
    [NativeCall(EntryPoint = "LuminaSharp_Net_GetConnections")] public static partial int GetConnections(ulong World, IntPtr Out, int Capacity);
    [NativeCall(EntryPoint = "LuminaSharp_Net_IsEntityNetworked")] public static partial int IsEntityNetworked(ulong World, uint Entity);
    [NativeCall(EntryPoint = "LuminaSharp_Net_GetOwner")] public static partial uint GetOwner(ulong World, uint Entity);
    [NativeCall(EntryPoint = "LuminaSharp_Net_SetOwner")] public static partial int SetOwner(ulong World, uint Entity, uint ConnectionId);
    [NativeCall(EntryPoint = "LuminaSharp_Net_MarkScriptDirty")] public static partial void MarkScriptDirty(IntPtr Script);
    [NativeCall(EntryPoint = "LuminaSharp_Net_MarkEntityDirty")] public static partial void MarkEntityDirty(ulong World, uint Entity);
    [NativeCall(EntryPoint = "LuminaSharp_Net_EntityToNetId")] public static partial uint EntityToNetId(ulong World, uint Entity);
    [NativeCall(EntryPoint = "LuminaSharp_Net_NetIdToEntity")] public static partial uint NetIdToEntity(ulong World, uint NetId);
    [NativeCall(EntryPoint = "LuminaSharp_Net_SendRpc")] public static partial int SendRpc(IntPtr Script, uint RpcId, uint Target, uint Flags, IntPtr Payload, int PayloadSize);
}
