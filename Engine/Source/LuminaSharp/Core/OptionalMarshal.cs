using System.Runtime.CompilerServices;

namespace LuminaSharp;

// The payload half of an optional slot, kept behind the struct constraint an unconstrained caller cannot express.
internal static unsafe class OptionalMarshal
{
    public static TPayload? Read<TPayload>(nint Payload) where TPayload : struct
    {
        return Payload == 0 ? null : Unsafe.ReadUnaligned<TPayload>((void*)Payload);
    }

    public static bool Stage<TPayload>(TPayload? Value, nint Scratch) where TPayload : struct
    {
        if (!Value.HasValue)
        {
            return false;
        }

        Unsafe.WriteUnaligned((void*)Scratch, Value.Value);
        return true;
    }

    public static int SizeOf<TPayload>() where TPayload : struct
    {
        return Unsafe.SizeOf<TPayload>();
    }
}
