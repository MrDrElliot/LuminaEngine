using System;
using System.Threading;
using System.Threading.Tasks;

using Lumina;

namespace LuminaSharp;

/// <summary>
/// Game-thread <c>await</c> helpers (s&amp;box-style). These complete from game-thread callbacks (the world
/// timer / async asset load), so the continuation resumes on the game thread with the ambient world restored
///, it is safe to touch the world after the await. Pass an <c>EntityScript.DestroyToken</c> so a pending
/// await cancels when the script is destroyed. Do NOT use these from a worker Task body.
/// </summary>
public static class GameTask
{
    // Registered so a generation unload or its world's teardown completes it, rather than leaving the await site never to resume.
    private sealed class FPending<T> : GameTaskRegistry.ICancelPending
    {
        internal readonly TaskCompletionSource<T> Source = new();

        // The callback context at the await, so the continuation sees the same world, entity and script.
        internal readonly Game.Scope Context = Game.Snapshot();

        private CancellationTokenRegistration Registration;
        private Lumina.CWorld? TimerWorld;
        private uint Timer;
        private int bDone;

        public ulong World { get; }

        internal FPending(Lumina.CWorld? InWorld)
        {
            World = InWorld != null ? InWorld.WorldHandle : 0;
            GameTaskRegistry.Track(this);
        }

        internal void WaitOnTimer(Lumina.CWorld InWorld, uint InTimer)
        {
            TimerWorld = InWorld;
            Timer = InTimer;
        }

        internal void Settle(T Value)
        {
            if (Finish())
            {
                using (Game.Resume(Context))
                {
                    Source.TrySetResult(Value);
                }
            }
        }

        public void Cancel()
        {
            if (!Finish())
            {
                return;
            }

            // A torn-down world has already cleared its timers, and clearing one there would reach a dead world.
            if (TimerWorld != null && Timer != 0 && TimerWorld.IsValid)
            {
                CTimerLibrary.Clear(TimerWorld, Timer);
            }
            Source.TrySetCanceled();
        }

        // Explicit tokens win, and otherwise the calling script's lifetime bounds the await.
        internal void CancelOn(CancellationToken Token)
        {
            if (!Token.CanBeCanceled && Game.ActiveScript is { } Owner)
            {
                Token = Owner.LifetimeToken;
            }
            if (Token.CanBeCanceled)
            {
                Registration = Token.Register(Cancel);
            }
        }

        // Exactly one of Settle and Cancel wins, and the winner drops the registration so a per-frame await cannot pile them up.
        private bool Finish()
        {
            if (Interlocked.Exchange(ref bDone, 1) != 0)
            {
                return false;
            }
            GameTaskRegistry.Forget(this);
            Registration.Dispose();
            return true;
        }
    }

    /// <summary>Resume after <paramref name="Seconds"/> of world time.</summary>
    public static System.Threading.Tasks.Task DelaySeconds(float Seconds, CancellationToken Token = default)
    {
        if (!Game.InWorld)
        {
            FPending<bool> Orphan = new(null);
            Orphan.Cancel();
            return Orphan.Source.Task;
        }

        Lumina.CWorld World = Game.World;
        FPending<bool> Pending = new(World);
        Pending.CancelOn(Token);
        uint Timer = CTimerLibrary.SetTimer(World, Seconds, ScriptCallback.OfRepeating(() => Pending.Settle(true)));
        Pending.WaitOnTimer(World, Timer);
        return Pending.Source.Task;
    }

    /// <summary>Resume on the next world tick.</summary>
    public static System.Threading.Tasks.Task NextFrame(CancellationToken Token = default) => DelaySeconds(0.0f, Token);

    /// <summary>Load an asset without blocking; resume with the result (or null) on the game thread.</summary>
    public static System.Threading.Tasks.Task<T?> LoadAsync<T>(string Path, CancellationToken Token = default) where T : NativeObject
    {
        FPending<T?> Pending = new(Game.InWorld ? Game.World : null);
        Pending.CancelOn(Token);
        Asset.LoadAsync<T>(Path, Result => Pending.Settle(Result));
        return Pending.Source.Task;
    }
}
