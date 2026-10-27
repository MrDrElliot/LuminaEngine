using LuminaSharp;

namespace Lumina;

// One stage scheduled worker over a world's component store, instanced once per world.
public partial class CEntitySystem
{
    // Valid from Configure onwards, read from the native object that owns the value for both languages.
    public CWorld World => GetWorld();


    // Runs after system T and anything deriving from it, in every stage both tick in.
    protected void RunsAfter<T>() where T : CEntitySystem => RunsAfterSystem(NativeTypeName.Of<T>());

    // Runs before system T and anything deriving from it, in every stage both tick in.
    protected void RunsBefore<T>() where T : CEntitySystem => RunsBeforeSystem(NativeTypeName.Of<T>());

    // Declares that this system writes component T, so it serializes against anything else reading or writing T.
    protected void Writes<T>() => DeclareWrite(typeof(T).Name);

    // Declares that this system only reads component T, so it runs concurrently with any other reader of T.
    protected void Reads<T>() => DeclareRead(typeof(T).Name);

    // Declares the read and hands back the reader that OnUpdate drains with Context.ReadEvents.
    protected LuminaSharp.EventReader<T> ReadsEvent<T>() where T : struct, LuminaSharp.IEvent
    {
        DeclareRead(LuminaSharp.EventType<T>.Name);
        return default;
    }

    // Declares that OnUpdate sends T with Context.SendEvent, so readers of T run after this system's batch.
    protected void SendsEvent<T>() where T : struct, LuminaSharp.IEvent => DeclareWrite(LuminaSharp.EventType<T>.Name);
}
