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
    // Registered so a generation unload completes it, rather than leaving the await site never to resume.
    private sealed class FPending<T> : GameTaskRegistry.ICancelPending
    {
        internal readonly TaskCompletionSource<T> Source = new();

        internal FPending()
        {
            GameTaskRegistry.Track(this);
        }

        internal void Settle(T Value)
        {
            GameTaskRegistry.Forget(this);
            Source.TrySetResult(Value);
        }

        public void Cancel()
        {
            GameTaskRegistry.Forget(this);
            Source.TrySetCanceled();
        }

        internal void CancelOn(CancellationToken Token)
        {
            if (Token.CanBeCanceled)
            {
                Token.Register(Cancel);
            }
        }
    }

    /// <summary>Resume after <paramref name="Seconds"/> of world time.</summary>
    public static System.Threading.Tasks.Task DelaySeconds(float Seconds, CancellationToken Token = default)
    {
        FPending<bool> Pending = new();
        if (!Game.InWorld)
        {
            Pending.Cancel();
            return Pending.Source.Task;
        }

        Lumina.CWorld World = Game.World;
        Entity Self = Game.CurrentEntity;
        CTimerLibrary.SetTimer(World, Seconds, ScriptCallback.OfRepeating(() =>
        {
            using (Game.Push(World, Self))
            {
                Pending.Settle(true);
            }
        }));

        Pending.CancelOn(Token);
        return Pending.Source.Task;
    }

    /// <summary>Resume on the next world tick.</summary>
    public static System.Threading.Tasks.Task NextFrame(CancellationToken Token = default) => DelaySeconds(0.0f, Token);

    /// <summary>Load an asset without blocking; resume with the result (or null) on the game thread.</summary>
    public static System.Threading.Tasks.Task<T?> LoadAsync<T>(string Path, CancellationToken Token = default) where T : NativeObject
    {
        FPending<T?> Pending = new();
        Lumina.CWorld? World = Game.InWorld ? Game.World : null;
        Entity Self = Game.CurrentEntity;

        Asset.LoadAsync<T>(Path, Result =>
        {
            if (World != null)
            {
                using (Game.Push(World, Self))
                {
                    Pending.Settle(Result);
                }
            }
            else
            {
                Pending.Settle(Result);
            }
        });

        Pending.CancelOn(Token);
        return Pending.Source.Task;
    }
}
