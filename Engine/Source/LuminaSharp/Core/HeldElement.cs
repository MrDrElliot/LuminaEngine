using System;

namespace LuminaSharp;

// A ring of per-thread slots that a ref-returning accessor hands out for a value with no storage of its own to point at.
internal static class HeldElement<T>
{
    private const int SlotMask = 15;

    [ThreadStatic] private static T[]? Slots;
    [ThreadStatic] private static int NextSlot;

    // The ring keeps a few nearby results apart, so f(List[0], List[1]) with in parameters still sees both.
    public static ref T Hold(T Value)
    {
        T[] Ring = Slots ??= new T[SlotMask + 1];
        ref T Slot = ref Ring[NextSlot++ & SlotMask];
        Slot = Value;
        return ref Slot;
    }
}
