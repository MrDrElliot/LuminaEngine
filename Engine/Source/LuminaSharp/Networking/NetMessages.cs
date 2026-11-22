using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using LuminaSharp;

namespace LuminaSharp
{
    // An event that may cross the network. A field "public Connection Sender;" is filled in on arrival with where it came from.
    public interface INetMessage : IEvent
    {
    }

    internal static class NetMessageLayout
    {
        private const uint Marker = 0x9E3779B9u;
        private const int MaxDepth = 8;
        private const BindingFlags InstanceFields = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;

        // The native side rewrites Sender and translates entity ids in place, so it needs their byte offsets.
        internal static unsafe void Register<T>(uint TypeId) where T : struct
        {
            FieldInfo? SenderField = typeof(T).GetField("Sender", InstanceFields);
            uint SenderOffset = SenderField != null && SenderField.FieldType == typeof(Connection)
                ? OffsetOf<T>([SenderField, IdOf(typeof(Connection))])
                : uint.MaxValue;

            var EntityPaths = new List<FieldInfo[]>();
            CollectEntities(typeof(T), new List<FieldInfo>(), EntityPaths);
            uint[] Offsets = new uint[EntityPaths.Count];
            for (int Index = 0; Index < Offsets.Length; ++Index)
            {
                Offsets[Index] = OffsetOf<T>(EntityPaths[Index]);
            }

            fixed (uint* OffsetData = Offsets)
            {
                Lumina.CNetSubsystem.RegisterScriptMessage(TypeId, SenderOffset, (IntPtr)OffsetData, (uint)Offsets.Length);
            }
        }

        private static FieldInfo IdOf(Type Wrapper) => Wrapper.GetField("Id", InstanceFields)!;

        private static void CollectEntities(Type Type, List<FieldInfo> Path, List<FieldInfo[]> OutPaths)
        {
            if (Path.Count >= MaxDepth)
            {
                return;
            }
            foreach (FieldInfo Field in Type.GetFields(InstanceFields))
            {
                Type FieldType = Field.FieldType;
                if (FieldType == typeof(FEntity))
                {
                    OutPaths.Add([.. Path, Field, IdOf(typeof(FEntity))]);
                }
                else if (FieldType.IsValueType && !FieldType.IsPrimitive && !FieldType.IsEnum && !FieldType.IsPointer && FieldType != typeof(Connection))
                {
                    Path.Add(Field);
                    CollectEntities(FieldType, Path, OutPaths);
                    Path.RemoveAt(Path.Count - 1);
                }
            }
        }

        // Writes a marker through the field path of an otherwise zeroed T and finds where it landed, which holds for any layout.
        private static uint OffsetOf<T>(FieldInfo[] Path) where T : struct
        {
            object Boxed = default(T);
            FieldInfo Leaf = Path[^1];
            TypedReference Reference = TypedReference.MakeTypedReference(Boxed, Path[..^1]);
            Leaf.SetValueDirect(Reference, Marker);

            T Value = (T)Boxed;
            ReadOnlySpan<byte> Bytes = MemoryMarshal.CreateReadOnlySpan(ref Unsafe.As<T, byte>(ref Value), Unsafe.SizeOf<T>());
            for (int Offset = 0; Offset + sizeof(uint) <= Bytes.Length; ++Offset)
            {
                if (MemoryMarshal.Read<uint>(Bytes[Offset..]) == Marker)
                {
                    return (uint)Offset;
                }
            }
            return uint.MaxValue;
        }
    }
}

namespace Lumina
{
    public unsafe partial class CNetSubsystem
    {
        // Raised as an event of T where Target says, read like any other event. Client names the connection for ENetMessageTarget.Client.
        public bool Send<T>(ENetMessageTarget Target, in T Message, Connection Client = default) where T : unmanaged, INetMessage
        {
            fixed (T* Pointer = &Message)
            {
                return SendScriptMessage((uint)Target, Client.Id, EventType<T>.Id, (IntPtr)Pointer);
            }
        }
    }
}
