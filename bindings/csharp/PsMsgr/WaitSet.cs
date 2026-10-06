// SPDX-License-Identifier: Apache-2.0
using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;

namespace PsMsgr;

/// <summary>
/// A <c>psmsgr_waitset</c> and the background thread that waits on it, which completes
/// the <see cref="StateReader.WaitAsync(uint, TimeSpan, CancellationToken)"/> calls
/// registered with it. A set holds up to <see cref="Native.WaitSetMax"/> readers; more
/// concurrent waits open more sets. Sets and their threads are created on demand and kept
/// for the life of the process. Without <c>futex_waitv</c> each wait gets a thread of its
/// own instead (spec/bindings.md).
/// </summary>
internal sealed unsafe class WaitSet
{
    // Guards the list of sets and every set's table, count and deadline.
    private static readonly object Gate = new();
    private static readonly List<WaitSet> Sets = [];
    // Null until the first open; then whether the kernel has futex_waitv.
    private static bool? HasWaitSets;
    private static long LastToken;

    private readonly Dictionary<ulong, AsyncWait> _waits = [];
    // Registrations, including those being added: at most WaitSetMax.
    private int _count;
    // What the waiting thread waits toward (MaxValue: no deadline).
    private ulong _deadline = ulong.MaxValue;

    private WaitSet(IntPtr ws) => Ptr = ws;

    internal IntPtr Ptr { get; }

    /// <summary>Waitsets open so far.</summary>
    internal static int Count
    {
        get
        {
            lock (Gate)
                return Sets.Count;
        }
    }

    /// <summary>Whether the library has waitsets here; null before the first async wait.</summary>
    internal static bool? Supported
    {
        get
        {
            lock (Gate)
                return HasWaitSets;
        }
    }

    /// <summary>Waits for a change of <paramref name="reader"/> after its first check
    /// found none. <paramref name="deadline"/> is in <see cref="Clock.NowNs"/> time,
    /// <c>ulong.MaxValue</c> for none.</summary>
    internal static Task<bool> Start(StateReader reader, uint lastGeneration, ulong deadline,
        CancellationToken cancellationToken, bool threadPerWait)
    {
        WaitSet? set = threadPerWait ? null : Reserve();
        if (set is null)
            return AsyncWait.StartThread(reader, lastGeneration, deadline, cancellationToken);

        ulong token = (ulong)Interlocked.Increment(ref LastToken);
        var w = new AsyncWait(reader, set, token, deadline, cancellationToken);
        IntPtr h;
        try
        {
            h = reader.BeginAsyncWait(w, addRef: true);
        }
        catch (ObjectDisposedException)
        {
            // Disposed by another thread since WaitAsync checked.
            lock (Gate)
                set._count--;
            throw;
        }
        w.SetHandle(h);
        lock (Gate)
            set._waits.Add(token, w);
        // The table entry comes first: the event may arrive before add returns.
        int rc = Native.psmsgr_waitset_add(set.Ptr, h, lastGeneration, token);
        if (rc != Native.Ok)
        {
            w.Fail(PsMsgrException.FromResult(rc, reader.Name));
            return w.Task;
        }
        if (set.MarkRegistered(w))
        {
            if (cancellationToken.CanBeCanceled)
                w.WatchCancellation();
            // A Dispose on another thread that came too early to remove it.
            if (reader.IsDisposed)
                w.Abort(AsyncWait.Outcome.Disposed);
        }
        return w.Task;
    }

    // A set with room for one more reader, counted; null without futex_waitv.
    private static WaitSet? Reserve()
    {
        lock (Gate)
        {
            if (HasWaitSets == false)
                return null;
            foreach (WaitSet s in Sets)
            {
                if (s._count < Native.WaitSetMax)
                {
                    s._count++;
                    return s;
                }
            }
            IntPtr ws;
            int rc = Native.psmsgr_waitset_open(&ws);
            if (rc == (int)PsMsgrError.NotSup)
            {
                HasWaitSets = false;
                return null;
            }
            if (rc != Native.Ok)
                throw PsMsgrException.FromResult(rc, null);
            HasWaitSets = true;
            var set = new WaitSet(ws) { _count = 1 };
            Sets.Add(set);
            StartThread(set.Run, "PsMsgr.WaitAsync");
            return set;
        }
    }

    /// <summary>Starts a background thread without the caller's
    /// <see cref="ExecutionContext"/>, so that a thread that outlives the call does not keep
    /// the caller's <see cref="AsyncLocal{T}"/> values (an Activity, a logging scope)
    /// reachable.</summary>
    internal static Thread StartThread(ThreadStart run, string name)
    {
        var t = new Thread(run) { IsBackground = true, Name = name };
        if (ExecutionContext.IsFlowSuppressed())
        {
            t.Start();
        }
        else
        {
            using (ExecutionContext.SuppressFlow())
                t.Start();
        }
        return t;
    }

    // After the add: makes the registration one that a timeout, cancellation or Dispose
    // may remove, and wakes the waiting thread if its deadline is earlier than the one it
    // waits toward. False if an event finished it already.
    internal bool MarkRegistered(AsyncWait w)
    {
        bool wake;
        lock (Gate)
        {
            if (!w.TryRegister())
                return false;
            wake = w.Deadline < _deadline;
        }
        if (wake)
            Native.psmsgr_waitset_wake(Ptr);
        return true;
    }

    internal void Forget(AsyncWait w)
    {
        lock (Gate)
        {
            _waits.Remove(w.Token);
            _count--;
        }
    }

    // The waiting thread. It never ends: the set stays for the life of the process.
    private void Run()
    {
        NativeWaitSetEvent* events = stackalloc NativeWaitSetEvent[Native.WaitSetMax];
        var due = new List<AsyncWait>();
        var reported = new List<(AsyncWait, NativeWaitSetEvent)>();
        while (true)
        {
            ulong now = Native.psmsgr_now_ns();
            ulong next = ulong.MaxValue;
            lock (Gate)
            {
                foreach (AsyncWait w in _waits.Values)
                {
                    if (!w.IsWaiting)
                        continue;
                    if (w.Deadline <= now && w.IsRegistered)
                        due.Add(w);
                    else if (w.Deadline < next)
                        next = w.Deadline;
                }
                _deadline = due.Count > 0 ? now : next;
            }
            if (due.Count > 0)
            {
                foreach (AsyncWait w in due)
                    w.Abort(AsyncWait.Outcome.Timeout);
                due.Clear();
                continue;
            }
            int ms = -1;
            if (next != ulong.MaxValue)
            {
                // Rounded up, so that a positive remainder never becomes a poll.
                ulong remaining = next <= now ? 0 : (next - now + 999_999) / 1_000_000;
                ms = (int)Math.Min(remaining, (ulong)int.MaxValue);
            }
            uint n;
            int rc = Native.psmsgr_waitset_wait(Ptr, ms, events, Native.WaitSetMax, &n);
            if (rc == Native.Ok)
            {
                lock (Gate)
                {
                    for (uint i = 0; i < n; i++)
                    {
                        if (_waits.TryGetValue(events[i].Token, out AsyncWait? w))
                            reported.Add((w, events[i]));
                    }
                }
                foreach (var (w, e) in reported)
                    w.Report(e);
                reported.Clear();
            }
            else if (rc != (int)PsMsgrError.Timeout && rc != (int)PsMsgrError.Intr)
            {
                // Not expected (futex_waitv failing otherwise): the registered waits fail
                // with the error, and the thread carries on after a pause.
                var error = PsMsgrException.FromResult(rc, null);
                lock (Gate)
                    due.AddRange(_waits.Values);
                foreach (AsyncWait w in due)
                    w.Abort(AsyncWait.Outcome.Error, error);
                due.Clear();
                Thread.Sleep(100);
            }
            // TIMEOUT, INTR (the runtime signals threads too), or a wake: scan again.
        }
    }
}

/// <summary>One <see cref="StateReader.WaitAsync(uint, TimeSpan, CancellationToken)"/>:
/// a waitset registration, or a thread of its own.</summary>
internal sealed class AsyncWait
{
    internal enum Outcome { Timeout, Canceled, Disposed, Error }

    // Adding -> Registered -> (Claimed ->) Done. Claimed: a remove is in progress, by the
    // timeout, the cancellation or Dispose, whichever claimed it first. An event finishes
    // it from any state.
    private const int Adding = 0, Registered = 1, Claimed = 2, Done = 3;

    private readonly TaskCompletionSource<bool> _tcs = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly StateReader _reader;
    private readonly WaitSet? _set;
    private readonly CancellationToken _cancellationToken;
    private CancellationTokenRegistration _ctr; // under lock (_tcs), set unless Done
    private IntPtr _handle;
    private int _state;

    internal AsyncWait(StateReader reader, WaitSet? set, ulong token, ulong deadline, CancellationToken cancellationToken)
    {
        _reader = reader;
        _set = set;
        Token = token;
        Deadline = deadline;
        _cancellationToken = cancellationToken;
    }

    internal ulong Token { get; }
    internal ulong Deadline { get; }
    internal Task<bool> Task => _tcs.Task;
    internal bool IsWaiting => Volatile.Read(ref _state) <= Registered;
    internal bool IsRegistered => Volatile.Read(ref _state) == Registered;

    internal void SetHandle(IntPtr h) => _handle = h;

    internal static Task<bool> StartThread(StateReader reader, uint lastGeneration, ulong deadline, CancellationToken cancellationToken)
    {
        var w = new AsyncWait(reader, null, 0, deadline, cancellationToken);
        reader.BeginAsyncWait(w, addRef: false);
        WaitSet.StartThread(() => w.RunBlocking(lastGeneration), "PsMsgr.Wait");
        return w.Task;
    }

    // Without waitsets: the synchronous wait, which checks the token and Dispose every 100 ms.
    private void RunBlocking(uint lastGeneration)
    {
        bool changed;
        try
        {
            changed = _reader.WaitUntil(lastGeneration, Deadline, _cancellationToken);
        }
        catch (OperationCanceledException) when (_cancellationToken.IsCancellationRequested)
        {
            if (TryFinish())
                _tcs.TrySetCanceled(_cancellationToken);
            return;
        }
        catch (Exception e)
        {
            Fail(e);
            return;
        }
        if (TryFinish())
            _tcs.TrySetResult(changed);
    }

    internal bool TryRegister() => Interlocked.CompareExchange(ref _state, Registered, Adding) == Adding;

    internal void WatchCancellation()
    {
        CancellationTokenRegistration ctr = _cancellationToken.Register(
            static s => ((AsyncWait)s!).Abort(Outcome.Canceled), this);
        bool done;
        lock (_tcs)
        {
            done = _state == Done;
            if (!done)
                _ctr = ctr;
        }
        if (done)
            ctr.Dispose();
    }

    /// <summary>Ends a registered wait by removing it from the set. If the set has already
    /// reported it, the event is the answer instead. No-op with a thread per wait: that
    /// thread sees the cancellation and Dispose itself.</summary>
    internal void Abort(Outcome how, Exception? error = null)
    {
        if (_set is null || Interlocked.CompareExchange(ref _state, Claimed, Registered) != Registered)
            return;
        // Blocks while the waiting thread scans the set, at most briefly. NODATA: reported,
        // and the waiting thread finishes it with the event.
        if (Native.psmsgr_waitset_remove(_set.Ptr, _handle) != Native.Ok || !TryFinish())
            return;
        switch (how)
        {
            case Outcome.Timeout:
                _tcs.TrySetResult(false);
                break;
            case Outcome.Canceled:
                _tcs.TrySetCanceled(_cancellationToken);
                break;
            case Outcome.Disposed:
                _tcs.TrySetException(new ObjectDisposedException(nameof(StateReader)));
                break;
            default:
                _tcs.TrySetException(error!);
                break;
        }
    }

    // On the waiting thread: the set reported the reader, which is the caller's again.
    internal void Report(in NativeWaitSetEvent e)
    {
        if (!TryFinish())
            return;
        if (e.Status == Native.Ok)
            _tcs.TrySetResult(true);
        else
            _tcs.TrySetException(new PsMsgrException((PsMsgrError)e.Status, errno: e.SysErrno, channelName: _reader.Name));
    }

    internal void Fail(Exception e)
    {
        if (TryFinish())
            _tcs.TrySetException(e);
    }

    // Hands the reader back; true for the one caller that then completes the task.
    private bool TryFinish()
    {
        if (Interlocked.Exchange(ref _state, Done) == Done)
            return false;
        _set?.Forget(this);
        CancellationTokenRegistration ctr;
        lock (_tcs)
            ctr = _ctr;
        _reader.EndAsyncWait(release: _set is not null);
        // Waits for a cancellation callback running on another thread, which has lost the
        // claim or got NODATA, so it returns at once. From inside the callback, no wait.
        ctr.Dispose();
        return true;
    }
}
