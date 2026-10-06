// SPDX-License-Identifier: Apache-2.0
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Text;

namespace PsMsgr.Tests;

/// <summary>StateReader.WaitAsync, on waitsets and (threadPerWait) the fallback without
/// futex_waitv, which the internal overload forces.</summary>
// Not in parallel with StateTests: a child process it starts holds this class's
// channel lock files until its exec, and an Unlink then fails with WriterExists.
[Collection("Channels")]
public sealed class WaitAsyncTests : ChannelTest
{
    private static readonly TimeSpan Long = TimeSpan.FromSeconds(10);

    private static byte[] B(string s) => Encoding.UTF8.GetBytes(s);

    // A task that WaitAsync completed before returning.
    private static async Task<bool> Completed(Task<bool> task)
    {
        Assert.True(task.IsCompletedSuccessfully);
        return await task;
    }

    private static void SkipWithoutWaitSets(bool threadPerWait)
    {
        if (!threadPerWait && WaitSet.Supported == false)
            Assert.Skip("no futex_waitv here: WaitAsync uses a thread per wait");
    }

    [Fact]
    public async Task WaitSetsAvailable()
    {
        using var r = OpenReader();
        Assert.False(await r.WaitAsync(0, TimeSpan.FromMilliseconds(1)));
        Assert.NotNull(WaitSet.Supported);
        if (WaitSet.Supported == false)
            Assert.Skip("no futex_waitv here (Linux < 5.16, qemu-user or seccomp): WaitAsync uses a thread per wait");
        Assert.True(WaitSet.Count >= 1);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task WakesOnPublish(bool threadPerWait)
    {
        using var w = OpenWriter(8, slotCount: 2);
        using var r = OpenReader();
        uint gen = w.Publish(B("a"));
        // A value that differs from lastGeneration completes at once, on any timeout.
        Task<bool> done = r.WaitAsync(0, TimeSpan.Zero, default, threadPerWait);
        Assert.True(await Completed(done));
        done = r.WaitAsync(GenAfter(gen, 1), Timeout.InfiniteTimeSpan, default, threadPerWait);
        Assert.True(await Completed(done));

        Task<bool> waiting = r.WaitAsync(gen, Long, default, threadPerWait);
        await Task.Delay(50);
        Assert.False(waiting.IsCompleted);
        // The reader belongs to the wait meanwhile.
        Assert.Throws<InvalidOperationException>(() => r.TryPeek(out _));
        Assert.Throws<InvalidOperationException>(() => r.Wait(gen, TimeSpan.Zero));
        Assert.Throws<InvalidOperationException>(() => { _ = r.WaitAsync(gen, Long); });
        Assert.Throws<InvalidOperationException>(() => r.IsWriterAlive);
        w.Publish(B("b"));
        Assert.True(await waiting.WaitAsync(Long));
        Assert.True(r.TryPeek(out StateInfo info));
        Assert.Equal(GenAfter(gen, 1), info.Generation);

        // Again on the same reader, following the value.
        waiting = r.WaitAsync(info.Generation, Long, default, threadPerWait);
        w.Publish(B("c"));
        Assert.True(await waiting.WaitAsync(Long));
        Assert.True(r.Read(out info) is [(byte)'c']);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Timeouts(bool threadPerWait)
    {
        using var w = OpenWriter(8, slotCount: 2);
        using var r = OpenReader();
        Task<bool> done = r.WaitAsync(0, TimeSpan.Zero, default, threadPerWait); // nothing published
        Assert.False(await Completed(done));
        uint gen = w.Publish(B("a"));
        done = r.WaitAsync(gen, TimeSpan.Zero, default, threadPerWait);
        Assert.False(await Completed(done));

        var sw = Stopwatch.StartNew();
        Assert.False(await r.WaitAsync(gen, TimeSpan.FromMilliseconds(120), default, threadPerWait));
        Assert.True(sw.Elapsed >= TimeSpan.FromMilliseconds(120));

        // Below a millisecond: rounded up, not a poll.
        sw.Restart();
        done = r.WaitAsync(gen, TimeSpan.FromTicks(1), default, threadPerWait);
        Assert.False(await done);
        Assert.True(sw.Elapsed >= TimeSpan.FromTicks(1));

        using (var absent = OpenReader("absent"))
        {
            sw.Restart();
            Assert.False(await absent.WaitAsync(0, TimeSpan.FromMilliseconds(50), default, threadPerWait));
            Assert.True(sw.Elapsed >= TimeSpan.FromMilliseconds(50));
        }

        // A later wait with an earlier deadline ends first.
        using (var r2 = OpenReader())
        {
            sw.Restart();
            Task<bool> late = r.WaitAsync(gen, TimeSpan.FromMilliseconds(400), default, threadPerWait);
            Task<bool> early = r2.WaitAsync(gen, TimeSpan.FromMilliseconds(100), default, threadPerWait);
            Assert.False(await early);
            TimeSpan earlyAt = sw.Elapsed;
            Assert.False(late.IsCompleted);
            Assert.False(await late);
            Assert.InRange(earlyAt, TimeSpan.FromMilliseconds(100), TimeSpan.FromMilliseconds(390));
            Assert.True(sw.Elapsed >= TimeSpan.FromMilliseconds(400));
        }

        // A timeout beyond the C API's int32 milliseconds still waits (for a publish here).
        Task<bool> waiting = r.WaitAsync(gen, TimeSpan.MaxValue, default, threadPerWait);
        await Task.Delay(50);
        Assert.False(waiting.IsCompleted);
        w.Publish(B("b"));
        Assert.True(await waiting.WaitAsync(Long));

        Assert.Throws<ArgumentOutOfRangeException>(() => { _ = r.WaitAsync(gen, TimeSpan.FromMilliseconds(-2), default, threadPerWait); });
        Assert.Throws<ArgumentOutOfRangeException>(() => { _ = r.WaitAsync(gen, TimeSpan.FromTicks(-1), default, threadPerWait); });
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Cancellation(bool threadPerWait)
    {
        using var w = OpenWriter(8, slotCount: 2);
        using var r = OpenReader();
        uint gen = w.Publish(B("a"));

        using var canceled = new CancellationTokenSource();
        canceled.Cancel();
        Task<bool> done = r.WaitAsync(0, TimeSpan.Zero, canceled.Token, threadPerWait);
        Assert.True(done.IsCanceled);

        foreach (TimeSpan timeout in new[] { Timeout.InfiniteTimeSpan, TimeSpan.FromSeconds(30) })
        {
            var sw = Stopwatch.StartNew();
            using var cts = new CancellationTokenSource(TimeSpan.FromMilliseconds(150));
            var e = await Assert.ThrowsAnyAsync<OperationCanceledException>(
                () => r.WaitAsync(gen, timeout, cts.Token, threadPerWait).WaitAsync(Long));
            Assert.Equal(cts.Token, e.CancellationToken);
            // The thread per wait checks the token every 100 ms; the waitset at once. The
            // timer may fire a little early.
            Assert.InRange(sw.Elapsed, TimeSpan.FromMilliseconds(140), TimeSpan.FromSeconds(2));
            Assert.True(r.TryPeek(out _)); // the reader is the caller's again
        }

        // A cancel from another thread while the wait is in the kernel.
        using (var cts = new CancellationTokenSource())
        {
            Task<bool> waiting = r.WaitAsync(gen, Timeout.InfiniteTimeSpan, cts.Token, threadPerWait);
            await Task.Delay(50);
            await Task.Run(cts.Cancel);
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() => waiting.WaitAsync(Long));
        }

        // A cancelable wait still ends on time, and on a publish.
        using var never = new CancellationTokenSource();
        var watch = Stopwatch.StartNew();
        Assert.False(await r.WaitAsync(gen, TimeSpan.FromMilliseconds(250), never.Token, threadPerWait));
        Assert.True(watch.Elapsed >= TimeSpan.FromMilliseconds(250));
        Task<bool> published = r.WaitAsync(gen, Timeout.InfiniteTimeSpan, never.Token, threadPerWait);
        await Task.Delay(150);
        w.Publish(B("b"));
        Assert.True(await published.WaitAsync(Long));
        never.Cancel(); // after the result: no effect
        Assert.True(published.IsCompletedSuccessfully);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task DisposeEndsWait(bool threadPerWait)
    {
        using var w = OpenWriter(8);
        uint gen = w.Publish(B("a"));
        foreach (TimeSpan timeout in new[] { Timeout.InfiniteTimeSpan, TimeSpan.FromSeconds(30) })
        {
            var r = OpenReader();
            Task<bool> waiting = r.WaitAsync(gen, timeout, default, threadPerWait);
            await Task.Delay(50);
            var sw = Stopwatch.StartNew();
            await Task.Run(r.Dispose);
            await Assert.ThrowsAsync<ObjectDisposedException>(() => waiting.WaitAsync(Long));
            Assert.InRange(sw.Elapsed, TimeSpan.Zero, TimeSpan.FromSeconds(2));
            Assert.Throws<ObjectDisposedException>(() => r.TryPeek(out _));
            Assert.Throws<ObjectDisposedException>(() => { _ = r.WaitAsync(gen, TimeSpan.Zero, default, threadPerWait); });
            r.Dispose();
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task ErrorsMatchWait(bool threadPerWait)
    {
        // Found by the first check: a faulted task, not a throw.
        using (var w = OpenWriter(8, slotCount: 2, notify: false))
        using (var r = OpenReader())
        {
            w.Publish(B("a"));
            Task<bool> done = r.WaitAsync(0, Long, default, threadPerWait);
            Assert.True(done.IsFaulted);
            var e = await Assert.ThrowsAsync<PsMsgrException>(() => done);
            Assert.Equal(PsMsgrError.NotSup, e.Code);
            Assert.Equal(Chan, e.ChannelName);
        }
        Assert.True(Channel.Unlink(Chan, Dir));

        // Found while waiting: the channel appears without notification, or invalid.
        using (var r = OpenReader())
        {
            Task<bool> waiting = r.WaitAsync(0, Long, default, threadPerWait);
            await Task.Delay(50);
            Assert.False(waiting.IsCompleted);
            using var w = OpenWriter(8, slotCount: 2, notify: false);
            w.Publish(B("a"));
            var e = await Assert.ThrowsAsync<PsMsgrException>(() => waiting.WaitAsync(Long));
            Assert.Equal(PsMsgrError.NotSup, e.Code);
            Assert.Equal(Chan, e.ChannelName);
        }
        using (var r = OpenReader("junk"))
        {
            Task<bool> waiting = r.WaitAsync(0, Long, default, threadPerWait);
            await Task.Delay(50);
            string tmp = Path.Combine(Dir, "junk.tmp");
            File.WriteAllBytes(tmp, Enumerable.Repeat((byte)0xA5, 4096).ToArray());
            File.Move(tmp, DataPath("junk"));
            var e = await Assert.ThrowsAsync<PsMsgrException>(() => waiting.WaitAsync(Long));
            Assert.Equal(PsMsgrError.Format, e.Code);
        }
    }

    // Holding the sets' lock stops the waiting thread before it looks at the deadlines and
    // the set again. Meanwhile the deadline passes and the value changes: the thread then
    // removes the reader for the timeout before any scan, and only the check it makes at
    // the deadline, like Wait's, sees the change.
    private static void ChangeWhileHeld(Action change)
    {
        Monitor.Enter(WaitSet.Gate);
        try
        {
            Thread.Sleep(200); // past the deadline: the thread is back and blocked on the lock
            change();
        }
        finally
        {
            Monitor.Exit(WaitSet.Gate);
        }
    }

    [Fact]
    public async Task TimeoutChecksAtTheDeadline()
    {
        using (var w = OpenWriter(8, slotCount: 2))
        using (var r = OpenReader())
        {
            uint gen = w.Publish(B("a"));
            Task<bool> waiting = r.WaitAsync(gen, TimeSpan.FromMilliseconds(50));
            SkipWithoutWaitSets(false);
            ChangeWhileHeld(() => w.Publish(B("b")));
            Assert.True(await waiting.WaitAsync(Long));
            Assert.True(r.Read(out _) is [(byte)'b']);
        }

        // An error, as Wait's check would raise: a channel appeared without notification.
        using (var r = OpenReader("late"))
        {
            Task<bool> waiting = r.WaitAsync(0, TimeSpan.FromMilliseconds(50));
            StateWriter? w = null;
            try
            {
                ChangeWhileHeld(() =>
                {
                    w = OpenWriter(8, slotCount: 2, notify: false, name: "late");
                    w.Publish(B("a"));
                });
                var e = await Assert.ThrowsAsync<PsMsgrException>(() => waiting.WaitAsync(Long));
                Assert.Equal(PsMsgrError.NotSup, e.Code);
                Assert.Equal("late", e.ChannelName);
            }
            finally
            {
                w?.Dispose();
            }
        }
    }

    [Theory]
    [InlineData(false, 300)] // three waitsets
    [InlineData(true, 20)]
    public async Task ManyReaders(bool threadPerWait, int count)
    {
        SkipWithoutWaitSets(threadPerWait);
        using var w = OpenWriter(8);
        uint gen = w.Publish(B("a"));
        var readers = Enumerable.Range(0, count).Select(_ => OpenReader()).ToList();
        using var quiet = OpenReader("quiet");
        try
        {
            // One more that times out among them.
            Task<bool> timesOut = quiet.WaitAsync(0, TimeSpan.FromMilliseconds(100), default, threadPerWait);
            var waits = readers.Select(r => r.WaitAsync(gen, Long, default, threadPerWait)).ToList();
            if (!threadPerWait)
                Assert.True(WaitSet.Count >= (count + Native.WaitSetMax - 1) / Native.WaitSetMax);
            Assert.False(await timesOut.WaitAsync(Long));
            await Task.Delay(50);
            Assert.DoesNotContain(waits, t => t.IsCompleted);

            // Continuations run elsewhere, even those that ask to run synchronously.
            var threads = waits.Select(t => t.ContinueWith(
                _ => Thread.CurrentThread.Name, CancellationToken.None,
                TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default)).ToList();
            w.Publish(B("b"));
            Assert.All(await Task.WhenAll(waits).WaitAsync(Long), Assert.True);
            Assert.All(await Task.WhenAll(threads), name => Assert.NotEqual("PsMsgr.WaitAsync", name));
            foreach (StateReader r in readers)
                Assert.True(r.TryPeek(out StateInfo info) && info.Generation == GenAfter(gen, 1));

            // Half of them again; the others are disposed while waiting.
            waits = readers.Select(r => r.WaitAsync(GenAfter(gen, 1), Long, default, threadPerWait)).ToList();
            for (int i = 0; i < count; i += 2)
                readers[i].Dispose();
            for (int i = 0; i < count; i += 2)
                await Assert.ThrowsAsync<ObjectDisposedException>(() => waits[i].WaitAsync(Long));
            w.Publish(B("c"));
            for (int i = 1; i < count; i += 2)
                Assert.True(await waits[i].WaitAsync(Long));
        }
        finally
        {
            foreach (StateReader r in readers)
                r.Dispose();
        }
    }

    [Fact]
    public async Task PendingWaitKeepsReaderAlive()
    {
        using var w = OpenWriter(8);
        uint gen = w.Publish(B("a"));
        Task<bool> waiting = StartUnreferenced(gen);
        for (int i = 0; i < 3; i++)
        {
            GC.Collect();
            GC.WaitForPendingFinalizers();
        }
        w.Publish(B("b"));
        Assert.True(await waiting.WaitAsync(Long));
    }

    // No local keeps the reader; only the pending wait does.
    private Task<bool> StartUnreferenced(uint gen) => OpenReader().WaitAsync(gen, Long);

    private static readonly AsyncLocal<object?> CallerScope = new();

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void BackgroundThreadsDropCallerContext(bool flowSuppressed)
    {
        // The waiting threads outlive the WaitAsync that starts them: they must not keep
        // its AsyncLocal values reachable.
        CallerScope.Value = new object();
        object? seen = "unset";
        Thread t;
        if (flowSuppressed)
        {
            using (ExecutionContext.SuppressFlow())
                t = WaitSet.StartThread(() => seen = CallerScope.Value, "PsMsgr.Test");
            Assert.False(ExecutionContext.IsFlowSuppressed());
        }
        else
        {
            t = WaitSet.StartThread(() => seen = CallerScope.Value, "PsMsgr.Test");
            Assert.False(ExecutionContext.IsFlowSuppressed());
        }
        Assert.True(t.Join(Long));
        Assert.Null(seen);
        Assert.NotNull(CallerScope.Value);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task CancelableWaitDropsCallerContext(bool threadPerWait)
    {
        // The cancellation callback outlives the WaitAsync too.
        using var w = OpenWriter(8);
        using var r = OpenReader();
        uint gen = w.Publish(B("a"));
        using var cts = new CancellationTokenSource();
        (Task<bool> waiting, WeakReference scope) = StartInScope(r, gen, cts.Token, threadPerWait);
        for (int i = 0; i < 3; i++)
        {
            GC.Collect();
            GC.WaitForPendingFinalizers();
        }
        Assert.False(scope.IsAlive);
        cts.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => waiting.WaitAsync(Long));
    }

    // Starts the wait with an AsyncLocal value that only the caller's context keeps.
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static (Task<bool>, WeakReference) StartInScope(StateReader r, uint gen, CancellationToken token, bool threadPerWait)
    {
        var value = new object();
        CallerScope.Value = value;
        Task<bool> waiting = r.WaitAsync(gen, Timeout.InfiniteTimeSpan, token, threadPerWait);
        CallerScope.Value = null;
        return (waiting, new WeakReference(value));
    }

    [Fact]
    public async Task DeadlineDuringAddDoesNotSpin()
    {
        using var w = OpenWriter(8);
        using var r = OpenReader();
        using var r2 = OpenReader();
        uint gen = w.Publish(B("a"));
        Assert.False(await r.WaitAsync(gen, TimeSpan.FromMilliseconds(1)));
        SkipWithoutWaitSets(false);
        // Its timeout makes the waiting thread scan the set while r is still being added,
        // past its deadline.
        Task<bool> other = r2.WaitAsync(gen, TimeSpan.FromMilliseconds(50));
        long cpu = -1;
        WaitSet.BeforeAdd = reader =>
        {
            if (reader != r)
                return;
            long start = WaitingThreadsCpu();
            Thread.Sleep(300);
            cpu = WaitingThreadsCpu() - start;
        };
        Task<bool> waiting;
        try
        {
            waiting = r.WaitAsync(gen, TimeSpan.FromTicks(1));
        }
        finally
        {
            WaitSet.BeforeAdd = null;
        }
        Assert.False(await other.WaitAsync(Long));
        Assert.False(await waiting.WaitAsync(Long));
        // In clock ticks (10 ms): the 300 ms spent waiting, not spinning.
        Assert.InRange(cpu, 0, 5);
    }

    // The /proc/self/task directories of the waitsets' threads. The kernel truncates thread
    // names to 15 bytes.
    private static IEnumerable<string> WaitingThreads() => Directory.GetDirectories("/proc/self/task")
        .Where(t => File.ReadAllText(Path.Combine(t, "comm")).TrimEnd('\n') == "PsMsgr.WaitAsyn");

    // utime + stime of the waitsets' threads, in clock ticks.
    private static long WaitingThreadsCpu() => WaitingThreads().Sum(t =>
    {
        string stat = File.ReadAllText(Path.Combine(t, "stat"));
        string[] f = stat[(stat.LastIndexOf(')') + 2)..].Split(' ');
        return long.Parse(f[11]) + long.Parse(f[12]);
    });

    [Fact]
    public async Task WaitingThreadResumesAfterSignals()
    {
        using var w = OpenWriter(8);
        using var r = OpenReader();
        uint gen = w.Publish(B("a"));
        var sw = Stopwatch.StartNew();
        Task<bool> waiting = r.WaitAsync(gen, TimeSpan.FromMilliseconds(300));
        SkipWithoutWaitSets(false);
        int[] tids = WaitingThreads().Select(t => int.Parse(Path.GetFileName(t))).ToArray();
        Assert.NotEmpty(tids);
        int sent = 0;
        while (!waiting.IsCompleted)
        {
            foreach (int tid in tids)
                InterruptingSignal.Send(tid);
            sent++;
            await Task.Delay(20);
        }
        Assert.False(await waiting);
        Assert.True(sw.Elapsed >= TimeSpan.FromMilliseconds(300), $"returned after {sw.Elapsed}");
        Assert.True(sent >= 2);
    }
}
