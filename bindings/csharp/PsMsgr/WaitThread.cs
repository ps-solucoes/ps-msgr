// SPDX-License-Identifier: Apache-2.0
using System;
using System.Collections.Generic;
using System.Threading;

namespace PsMsgr;

/// <summary>
/// A thread that runs <see cref="StateReader.WaitAsync(uint, TimeSpan, CancellationToken)"/>
/// calls without <c>futex_waitv</c>, one at a time, blocked in each. Between them it waits
/// for the next one, and it ends after <see cref="IdleTimeout"/> without one. So a reader
/// that waits again and again reuses one thread instead of starting one per wait.
/// </summary>
internal sealed class WaitThread
{
    // The tests shorten it.
    internal static TimeSpan IdleTimeout = TimeSpan.FromSeconds(10);
    // The threads waiting for a wait, the most recently idle last, so that the others
    // reach their timeout.
    private static readonly List<WaitThread> Idle = [];
    private static int StartedCount;

    private AsyncWait? _next; // under lock (this)
    // IdleTimeout when the thread last became idle.
    private TimeSpan _idleTimeout = IdleTimeout;

    private WaitThread(AsyncWait first) => _next = first;

    /// <summary>Threads started so far.</summary>
    internal static int Started => Volatile.Read(ref StartedCount);

    /// <summary>Threads waiting for a wait now.</summary>
    internal static int IdleCount
    {
        get
        {
            lock (Idle)
                return Idle.Count;
        }
    }

    /// <summary>Runs <paramref name="w"/> on an idle thread, or on a new one if none is
    /// idle; throws if that cannot start.</summary>
    internal static void Dispatch(AsyncWait w)
    {
        WaitThread? t = null;
        lock (Idle)
        {
            if (Idle.Count > 0)
            {
                t = Idle[^1];
                Idle.RemoveAt(Idle.Count - 1);
            }
        }
        if (t is null)
        {
            WaitSet.StartThread(new WaitThread(w).Loop, "PsMsgr.Wait");
            Interlocked.Increment(ref StartedCount);
            return;
        }
        lock (t)
        {
            t._next = w;
            Monitor.Pulse(t);
        }
    }

    /// <summary>Called by the wait before it completes its task, so that a continuation's
    /// next wait finds this thread idle.</summary>
    internal void BecomeIdle()
    {
        lock (Idle)
        {
            _idleTimeout = IdleTimeout;
            Idle.Add(this);
        }
    }

    private void Loop()
    {
        while (RunNext())
        {
        }
    }

    // A separate frame, so that no local keeps the last wait (and its reader) reachable
    // while the thread is idle.
    private bool RunNext()
    {
        AsyncWait? w = Take();
        if (w is null)
            return false;
        w.RunBlocking(this);
        return true;
    }

    // The next wait, or null after IdleTimeout without one.
    private AsyncWait? Take()
    {
        lock (this)
        {
            while (_next is null)
            {
                if (Monitor.Wait(this, _idleTimeout) || _next is not null)
                    continue;
                lock (Idle)
                {
                    if (Idle.Remove(this))
                        return null;
                }
                // Dispatch took this thread meanwhile: its wait is on the way.
                while (_next is null)
                    Monitor.Wait(this);
            }
            AsyncWait w = _next;
            _next = null;
            return w;
        }
    }
}
