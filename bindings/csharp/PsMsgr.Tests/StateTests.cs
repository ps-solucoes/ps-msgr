// SPDX-License-Identifier: Apache-2.0
using System.Diagnostics;
using System.Text;

namespace PsMsgr.Tests;

/// <summary>The C unit tests (tests/test_state.c), where relevant, through the binding.</summary>
[Collection("Channels")]
public sealed class StateTests : ChannelTest
{
    private static byte[] B(string s) => Encoding.UTF8.GetBytes(s);

    private delegate void ScopeAction(WriteScope scope);

    // Lambdas cannot capture a ref struct.
    private static T Throws<T>(WriteScope scope, ScopeAction action) where T : Exception
    {
        try
        {
            action(scope);
        }
        catch (T e)
        {
            return e;
        }
        Assert.Fail($"no {typeof(T).Name}");
        return null!;
    }

    [Fact]
    public void CreateThenReuse()
    {
        var options = new StateOptions(16) { PayloadType = 0x1234, Mode = 0x1A0, Directory = Dir };
        uint first;
        ulong ino;
        using (var w = StateWriter.Open(Chan, options))
        {
            Assert.Equal(0x1A0, (int)File.GetUnixFileMode(DataPath()));
            Assert.Equal(0x1A0, (int)File.GetUnixFileMode(LockPath()));
            Assert.Equal(128 + 3 * 64, new FileInfo(DataPath()).Length);
            Assert.Equal(16u, w.Capacity);
            first = w.Publish(B("one"));
            Assert.NotEqual(0u, first);
            Assert.Equal(GenAfter(first, 1), w.Publish(B("two")));
            w.Publish(B("three"));
            ino = Inode(DataPath());
        }
        using (var w = StateWriter.Open(Chan, options))
        {
            Assert.Equal(ino, Inode(DataPath()));
            uint gen = w.Publish(B("four"));
            Assert.Equal(GenAfter(first, 3), gen);
            using var r = OpenReader();
            Assert.Equal(B("four"), r.Read(out StateInfo info));
            Assert.True(r.TryPeek(out StateInfo peeked));
            Assert.Equal(new StateInfo(gen, 4, peeked.TimestampNs, true), info);
            Assert.Equal(new ChannelDesc(16, 3, 0x1234, true), r.Describe());
        }
    }

    [Fact]
    public void MismatchAndRecreate()
    {
        uint oldGen;
        using (var w = OpenWriter(16))
            oldGen = w.Publish(B("old"));
        using var r = OpenReader();
        Assert.Equal(B("old"), r.Read(out StateInfo info));
        Assert.True(info.Attached);

        foreach (var options in new[]
        {
            new StateOptions(32),
            new StateOptions(16) { SlotCount = 4 },
            new StateOptions(16) { Notify = false },
            new StateOptions(16) { PayloadType = 9 },
        })
        {
            options.Directory = Dir;
            var e = Assert.Throws<PsMsgrException>(() => StateWriter.Open(Chan, options));
            Assert.Equal(PsMsgrError.Mismatch, e.Code);
        }
        Assert.True(r.TryPeek(out info));
        Assert.Equal(oldGen, info.Generation);

        ulong ino = Inode(DataPath());
        using (var w = OpenWriter(32, recreate: true))
        {
            Assert.NotEqual(ino, Inode(DataPath()));
            Assert.Equal(32u, w.Capacity);

            // The reader follows the retire to the new, still empty file.
            Assert.False(r.TryPeek(out _));
            Assert.Equal(32u, r.Describe()!.Value.Capacity);
            uint gen = w.Publish(B("new"));
            Assert.Equal(GenAfter(oldGen, 1), gen);
            Assert.Equal(B("new"), r.Read(out info));
            Assert.Equal((gen, true), (info.Generation, info.Attached));
        }

        // Recreate on a compatible file is a plain reuse.
        ino = Inode(DataPath());
        OpenWriter(32, recreate: true).Dispose();
        Assert.Equal(ino, Inode(DataPath()));
    }

    [Fact]
    public void SecondWriterSameProcess()
    {
        var a = OpenWriter(8, slotCount: 2);
        var e = Assert.Throws<PsMsgrException>(() => OpenWriter(8, slotCount: 2));
        Assert.Equal(PsMsgrError.WriterExists, e.Code);
        Assert.Equal(Chan, e.ChannelName);
        Assert.Throws<PsMsgrException>(() => OpenWriter(8, slotCount: 2, recreate: true));
        e = Assert.Throws<PsMsgrException>(() => Channel.Unlink(Chan, Dir));
        Assert.Equal(PsMsgrError.WriterExists, e.Code);
        a.Dispose();
        OpenWriter(8, slotCount: 2).Dispose();
    }

    [Fact]
    public void SecondWriterOtherProcess()
    {
        using var r = OpenReader();
        Assert.False(r.IsWriterAlive); // no lock file yet
        using (var child = new ChildWriter(Dir, 5))
        {
            var e = Assert.Throws<PsMsgrException>(() => OpenWriter(24, payloadType: 0x0001_0001));
            Assert.Equal(PsMsgrError.WriterExists, e.Code);
            Assert.True(r.IsWriterAlive);
            child.Kill(); // SIGKILL: the lock goes with the process
        }
        Assert.False(r.IsWriterAlive);
        Assert.True(r.TryRead(out MotorStatus status, out _));
        Assert.Equal(5ul, status.Sequence);
        using (OpenWriter(24, payloadType: 0x0001_0001))
            Assert.True(r.IsWriterAlive);
        Assert.False(r.IsWriterAlive);
    }

    [Fact]
    public void LazyReaderAttachesLater()
    {
        using var r = OpenReader();
        Assert.Null(r.Read(out StateInfo info));
        Assert.Equal(default, info);
        Assert.False(r.TryRead(new byte[16], out _));
        Assert.False(r.TryRead(out MotorStatus _, out _));
        Assert.False(r.TryPeek(out _));
        Assert.Null(r.Describe());
        Assert.False(r.Wait(0, TimeSpan.Zero));
        Assert.False(r.IsWriterAlive);

        using var w = OpenWriter(8, slotCount: 2);
        Assert.False(r.TryPeek(out _)); // attached, nothing published
        Assert.Equal(8u, r.Describe()!.Value.Capacity);
        Assert.True(r.IsWriterAlive);
        uint gen = w.Publish(B("hi"));
        Assert.Equal(B("hi"), r.Read(out info));
        Assert.Equal((gen, 2u, true), (info.Generation, info.Length, info.Attached));
    }

    [Fact]
    public void ReadResultsAndSizes()
    {
        using var w = OpenWriter(8, slotCount: 2);
        using var r = OpenReader();
        var e = Assert.Throws<PsMsgrException>(() => w.Publish(B("123456789")));
        Assert.Equal(PsMsgrError.TooBig, e.Code);
        Assert.False(r.TryPeek(out _)); // failures publish nothing
        uint gen = w.Publish(B("12345678"));

        byte[] small = Enumerable.Repeat((byte)0xAA, 7).ToArray();
        e = Assert.Throws<PsMsgrException>(() => r.TryRead(small, out _));
        Assert.Equal(PsMsgrError.TooSmall, e.Code);
        Assert.Contains("8 bytes", e.Message);
        Assert.All(small, b => Assert.Equal(0xAA, b)); // nothing copied
        var buf = new byte[8];
        // The attach that the TOOSMALL result carried is reported now.
        Assert.True(r.TryRead(buf, out StateInfo info));
        Assert.True(r.TryPeek(out StateInfo peeked));
        Assert.Equal(new StateInfo(gen, 8, peeked.TimestampNs, true), info);
        Assert.Equal(B("12345678"), buf);
        Assert.True(r.TryRead(buf, out info));
        Assert.False(info.Attached);

        // Into the middle of a larger buffer.
        var big = new byte[16];
        Assert.True(r.TryRead(big.AsSpan(4, 8), out info));
        Assert.Equal(8u, info.Length);
        byte[] expected = [.. new byte[4], .. B("12345678"), .. new byte[4]];
        Assert.Equal(expected, big);

        w.Publish([]);
        Assert.True(r.TryRead([], out info));
        Assert.Equal(0u, info.Length);
        Assert.Empty(r.Read(out _)!);
    }

    [Fact]
    public void GenericRoundTrip()
    {
        Assert.Equal(24, System.Runtime.CompilerServices.Unsafe.SizeOf<MotorStatus>());
        using var w = OpenWriter(24, payloadType: 0x0001_0001);
        using var r = OpenReader();
        var status = new MotorStatus { Sequence = 1, SpeedRpm = 1500f, CurrentA = 2.5f, TemperatureC = 41f };
        uint gen = w.Publish(status);
        Assert.True(r.TryRead(out MotorStatus got, out StateInfo info));
        Assert.Equal(status, got);
        Assert.Equal((gen, 24u, true), (info.Generation, info.Length, info.Attached));
        Assert.Equal(0x0001_0001u, r.Describe()!.Value.PayloadType);

        // Publish<T> writes the same bytes as the span overload.
        byte[] raw = r.Read(out _)!;
        Assert.Equal(status, System.Runtime.InteropServices.MemoryMarshal.Read<MotorStatus>(raw));

        // A payload of another length does not fit the type, and reports the attach later.
        w.Publish(B("short"));
        using var r2 = OpenReader();
        var e = Assert.Throws<PsMsgrException>(() => r2.TryRead(out MotorStatus _, out _));
        Assert.Equal(PsMsgrError.Mismatch, e.Code);
        Assert.Contains("5 bytes", e.Message);
        Assert.True(r2.TryPeek(out info));
        Assert.True(info.Attached);
        w.Publish(new byte[24]);
        Assert.Throws<PsMsgrException>(() => r.TryRead(out long _, out _));
        Assert.True(r.TryRead(out got, out info));
        Assert.Equal(default, got);
        Assert.False(info.Attached);
    }

    [Fact]
    public void HeartbeatChannel()
    {
        using var w = OpenWriter(0, slotCount: 2);
        Assert.Equal(0u, w.Capacity);
        Assert.Equal(128 + 2 * 64, new FileInfo(DataPath()).Length);
        Assert.Equal(PsMsgrError.TooBig, Assert.Throws<PsMsgrException>(() => w.Publish(B("x"))).Code);
        uint first = w.Publish([]);
        uint gen = w.Publish(ReadOnlySpan<byte>.Empty);
        Assert.Equal(GenAfter(first, 1), gen);
        using var r = OpenReader();
        Assert.Empty(r.Read(out StateInfo info)!);
        Assert.Equal(gen, info.Generation);
        Assert.Equal(0u, r.Describe()!.Value.Capacity);
        using (var scope = w.Begin())
        {
            Assert.Equal(0, scope.Buffer.Length);
            Assert.Equal(GenAfter(gen, 1), scope.Commit(0));
        }
    }

    [Fact]
    public void ReceiveBufferFollowsCapacity()
    {
        using var r = OpenReader();
        using (var w = OpenWriter(8))
        {
            w.Publish(B("12345678"));
            Assert.Equal(B("12345678"), r.Read(out _));
            Assert.Equal(8, r.BufferLength);
        }
        using (var w = OpenWriter(64, recreate: true))
        {
            byte[] data = Enumerable.Range(0, 64).Select(i => (byte)i).ToArray();
            w.Publish(data);
            Assert.Equal(data, r.Read(out StateInfo info));
            Assert.True(info.Attached);
            Assert.Equal(64, r.BufferLength);
        }
        using (var w = OpenWriter(4, recreate: true))
        {
            w.Publish(B("abcd"));
            Assert.True(r.TryPeek(out StateInfo info));
            Assert.True(info.Attached);
            Assert.Equal(B("abcd"), r.Read(out info));
            Assert.False(info.Attached); // the peek reported it
            Assert.Equal(4, r.BufferLength);
        }
    }

    [Fact]
    public void PeekTimestamps()
    {
        using var w = OpenWriter(8);
        using var r = OpenReader();
        ulong prev = 0;
        uint first = 0;
        for (int i = 1; i <= 20; i++)
        {
            ulong before = Clock.NowNs();
            uint gen = w.Publish(BitConverter.GetBytes(i));
            ulong after = Clock.NowNs();
            if (i == 1)
                first = gen;
            Assert.Equal(GenAfter(first, i - 1), gen);
            Assert.True(r.TryPeek(out StateInfo info));
            Assert.Equal((gen, 4u), (info.Generation, info.Length));
            Assert.InRange(info.TimestampNs, before, after);
            Assert.True(info.TimestampNs >= prev);
            Assert.Equal(i == 1, info.Attached);
            Assert.InRange(info.Age, TimeSpan.Zero, TimeSpan.FromSeconds(10));
            prev = info.TimestampNs;
        }
    }

    [Fact]
    public async Task WaitWakesOnPublish()
    {
        using var w = OpenWriter(8, slotCount: 2);
        using var r = OpenReader();
        uint gen = w.Publish(B("a"));
        // A value that differs from lastGeneration returns at once.
        Assert.True(r.Wait(0, TimeSpan.Zero));
        Assert.True(r.Wait(GenAfter(gen, 1), Timeout.InfiniteTimeSpan));

        var waiting = Task.Run(() => r.Wait(gen, TimeSpan.FromSeconds(10)));
        await Task.Delay(50); // likely blocked by now; the test holds either way
        w.Publish(B("b"));
        Assert.True(await waiting);
        Assert.True(r.TryPeek(out StateInfo info));
        Assert.Equal(GenAfter(gen, 1), info.Generation);
    }

    [Fact]
    public async Task WaitTimeouts()
    {
        using var w = OpenWriter(8, slotCount: 2);
        using var r = OpenReader();
        Assert.False(r.Wait(0, TimeSpan.Zero)); // nothing published counts as unchanged
        uint gen = w.Publish(B("a"));
        Assert.False(r.Wait(gen, TimeSpan.Zero));

        var sw = Stopwatch.StartNew();
        Assert.False(r.Wait(gen, TimeSpan.FromMilliseconds(120)));
        Assert.True(sw.Elapsed >= TimeSpan.FromMilliseconds(120));

        // Below a millisecond: rounded up, not a poll.
        sw.Restart();
        Assert.False(r.Wait(gen, TimeSpan.FromTicks(1)));
        Assert.True(sw.Elapsed >= TimeSpan.FromTicks(1));

        using (var absent = OpenReader("absent"))
        {
            sw.Restart();
            Assert.False(absent.Wait(0, TimeSpan.FromMilliseconds(50)));
            Assert.True(sw.Elapsed >= TimeSpan.FromMilliseconds(50));
        }

        // A timeout beyond the C API's int32 milliseconds still waits (for a publish here).
        var waiting = Task.Run(() => r.Wait(gen, TimeSpan.MaxValue));
        await Task.Delay(50);
        w.Publish(B("b"));
        Assert.True(await waiting.WaitAsync(TimeSpan.FromSeconds(10)));

        Assert.Throws<ArgumentOutOfRangeException>(() => r.Wait(gen, TimeSpan.FromMilliseconds(-2)));
        Assert.Throws<ArgumentOutOfRangeException>(() => r.Wait(gen, TimeSpan.FromTicks(-1)));
    }

    [Fact]
    public async Task WaitCancellation()
    {
        using var w = OpenWriter(8, slotCount: 2);
        using var r = OpenReader();
        uint gen = w.Publish(B("a"));

        using var canceled = new CancellationTokenSource();
        canceled.Cancel();
        Assert.Throws<OperationCanceledException>(() => r.Wait(0, TimeSpan.Zero, canceled.Token));

        foreach (TimeSpan timeout in new[] { Timeout.InfiniteTimeSpan, TimeSpan.FromSeconds(30) })
        {
            using var cts = new CancellationTokenSource(TimeSpan.FromMilliseconds(150));
            var sw = Stopwatch.StartNew();
            var e = Assert.ThrowsAny<OperationCanceledException>(() => r.Wait(gen, timeout, cts.Token));
            Assert.Equal(cts.Token, e.CancellationToken);
            Assert.InRange(sw.Elapsed, TimeSpan.FromMilliseconds(150), TimeSpan.FromSeconds(2));
        }

        // A cancelable wait still ends on time, and on a publish.
        using var never = new CancellationTokenSource();
        var watch = Stopwatch.StartNew();
        Assert.False(r.Wait(gen, TimeSpan.FromMilliseconds(250), never.Token));
        Assert.True(watch.Elapsed >= TimeSpan.FromMilliseconds(250));
        var waiting = Task.Run(() => r.Wait(gen, Timeout.InfiniteTimeSpan, never.Token));
        await Task.Delay(150);
        w.Publish(B("b"));
        Assert.True(await waiting.WaitAsync(TimeSpan.FromSeconds(10)));
    }

    [Fact]
    public async Task DisposeStopsWait()
    {
        using var w = OpenWriter(8);
        uint gen = w.Publish(B("a"));
        foreach (TimeSpan timeout in new[] { Timeout.InfiniteTimeSpan, TimeSpan.FromSeconds(30) })
        {
            var r = OpenReader();
            var waiting = Task.Run(() => r.Wait(gen, timeout));
            await Task.Delay(50); // likely blocked by now; the test holds either way
            var sw = Stopwatch.StartNew();
            r.Dispose();
            await Assert.ThrowsAsync<ObjectDisposedException>(() => waiting.WaitAsync(TimeSpan.FromSeconds(10)));
            Assert.InRange(sw.Elapsed, TimeSpan.Zero, TimeSpan.FromSeconds(2));
            Assert.Throws<ObjectDisposedException>(() => r.IsWriterAlive);
            Assert.Throws<ObjectDisposedException>(() => r.Wait(gen, TimeSpan.Zero));
        }
    }

    [Fact]
    public void WaitResumesAfterSignals()
    {
        using var w = OpenWriter(8);
        using var r = OpenReader();
        uint gen = w.Publish(B("a"));
        int tid = 0;
        using var ready = new ManualResetEventSlim();
        TimeSpan elapsed = default;
        bool result = true;
        Exception? error = null;
        var waiter = new Thread(() =>
        {
            tid = InterruptingSignal.CurrentThreadId();
            ready.Set();
            var sw = Stopwatch.StartNew();
            try
            {
                result = r.Wait(gen, TimeSpan.FromMilliseconds(300));
            }
            catch (Exception e)
            {
                error = e;
            }
            elapsed = sw.Elapsed;
        });
        waiter.Start();
        ready.Wait();
        int sent = 0;
        while (waiter.IsAlive)
        {
            InterruptingSignal.Send(tid);
            sent++;
            Thread.Sleep(20);
        }
        waiter.Join();
        Assert.Null(error);
        Assert.False(result);
        Assert.True(elapsed >= TimeSpan.FromMilliseconds(300), $"returned after {elapsed}");
        Assert.True(sent >= 2);
    }

    [Fact]
    public async Task WaitFollowsRetire()
    {
        using var r = OpenReader();
        uint gen;
        using (var w = OpenWriter(8, slotCount: 2))
            gen = w.Publish(B("a"));
        Assert.True(r.Wait(0, TimeSpan.Zero)); // attach
        var waiting = Task.Run(() => r.Wait(gen, TimeSpan.FromSeconds(10)));
        await Task.Delay(50);
        using (var w = OpenWriter(16, slotCount: 2, recreate: true))
        {
            await Task.Delay(50);
            Assert.False(waiting.IsCompleted); // the retire alone does not return
            w.Publish(B("b"));
            Assert.True(await waiting.WaitAsync(TimeSpan.FromSeconds(10)));
        }
        Assert.Equal(16u, r.Describe()!.Value.Capacity);
    }

    [Fact]
    public async Task WaitFollowsUnlink()
    {
        using var r = OpenReader();
        uint gen;
        using (var w = OpenWriter(8, slotCount: 2))
            gen = w.Publish(B("a"));
        Assert.True(r.Wait(0, TimeSpan.Zero));
        var waiting = Task.Run(() => r.Wait(gen, TimeSpan.FromSeconds(10)));
        await Task.Delay(50);
        Assert.True(Channel.Unlink(Chan, Dir));
        Assert.False(File.Exists(DataPath()) || File.Exists(LockPath()));
        await Task.Delay(50);
        Assert.False(waiting.IsCompleted);
        uint newGen;
        using (var w = OpenWriter(8, slotCount: 2))
        {
            newGen = w.Publish(B("c")); // must not reuse gen
            Assert.True(await waiting.WaitAsync(TimeSpan.FromSeconds(10)));
        }
        Assert.True(r.TryPeek(out StateInfo info));
        Assert.Equal(newGen, info.Generation);
        Assert.True(Channel.Unlink(Chan, Dir));
        Assert.False(r.TryPeek(out _));
        Assert.False(Channel.Unlink(Chan, Dir));
    }

    [Fact]
    public void WaitNotSupportedWithoutNotify()
    {
        using var w = OpenWriter(8, slotCount: 2, notify: false);
        using var r = OpenReader();
        w.Publish(B("a"));
        var e = Assert.Throws<PsMsgrException>(() => r.Wait(0, TimeSpan.FromMilliseconds(100)));
        Assert.Equal(PsMsgrError.NotSup, e.Code);
        Assert.Equal(new ChannelDesc(8, 2, 0, false), r.Describe());
    }

    [Fact]
    public void WriterAlive()
    {
        using var r = OpenReader();
        Assert.False(r.IsWriterAlive);
        using (OpenWriter(8))
            Assert.True(r.IsWriterAlive);
        Assert.False(r.IsWriterAlive);
    }

    [Fact]
    public void AttachedFlagOncePerAttach()
    {
        using var r = OpenReader();
        StateInfo info;
        using (var w = OpenWriter(8, slotCount: 2))
        {
            w.Publish([]);
            Assert.NotNull(r.Describe()); // attaches, does not consume
            Assert.True(r.Wait(0, TimeSpan.Zero)); // neither does wait
            Assert.True(r.TryPeek(out info) && info.Attached);
            Assert.True(r.TryPeek(out info) && !info.Attached);
            r.Read(out info);
            Assert.False(info.Attached);
        }
        using (var w = OpenWriter(8, slotCount: 3, recreate: true))
        {
            Assert.False(r.TryPeek(out _));
            w.Publish([]);
            r.Read(out info);
            Assert.True(info.Attached);
            Assert.True(r.TryPeek(out info) && !info.Attached);
        }
    }

    [Fact]
    public void Unlink()
    {
        Assert.False(Channel.Unlink(Chan, Dir));
        using (OpenWriter(8))
            Assert.Equal(PsMsgrError.WriterExists, Assert.Throws<PsMsgrException>(() => Channel.Unlink(Chan, Dir)).Code);
        Assert.True(Channel.Unlink(Chan, Dir));
        Assert.False(File.Exists(DataPath()) || File.Exists(LockPath()));
        Assert.False(Channel.Unlink(Chan, Dir));
    }

    [Fact]
    public void BeginCommitAbort()
    {
        using var w = OpenWriter(16);
        using var r = OpenReader();
        uint gen;
        using (var scope = w.Begin())
        {
            Assert.Equal(16, scope.Buffer.Length);
            B("zero-copy").CopyTo(scope.Buffer);
            // A publish or a second begin while the scope is open.
            Assert.Equal(PsMsgrError.State, Assert.Throws<PsMsgrException>(() => w.Publish(B("x"))).Code);
            Assert.Equal(PsMsgrError.State, Assert.Throws<PsMsgrException>(() => w.Begin()).Code);
            gen = scope.Commit(9);
            Throws<InvalidOperationException>(scope, s => s.Commit(9));
            Throws<InvalidOperationException>(scope, s => _ = s.Buffer);
            scope.Dispose(); // a no-op after the commit
        }
        Assert.Equal(B("zero-copy"), r.Read(out StateInfo info));
        Assert.Equal(gen, info.Generation);

        // Dispose without a commit aborts: readers keep the last value.
        using (var scope = w.Begin())
            B("partial").CopyTo(scope.Buffer);
        Assert.Equal(B("zero-copy"), r.Read(out info));
        Assert.Equal(gen, info.Generation);

        // TOOBIG leaves the scope open.
        using (var scope = w.Begin())
        {
            Assert.Equal(PsMsgrError.TooBig, Throws<PsMsgrException>(scope, s => s.Commit(17)).Code);
            Throws<ArgumentOutOfRangeException>(scope, s => s.Commit(-1));
            B("sixteen bytes!!!").CopyTo(scope.Buffer);
            Assert.Equal(GenAfter(gen, 1), scope.Commit(16));
        }
        Assert.Equal(B("sixteen bytes!!!"), r.Read(out _));

        // Disposing the writer aborts the scope; disposing the scope then is a no-op.
        var w2 = OpenWriter(8, name: "other");
        var open = w2.Begin();
        w2.Dispose();
        Throws<ObjectDisposedException>(open, s => s.Commit(0));
        Throws<ObjectDisposedException>(open, s => _ = s.Buffer);
        open.Dispose();

        // A copy of a scope is the same scope.
        using (var scope = w.Begin())
        {
            WriteScope copy = scope;
            copy.Commit(0);
            Throws<InvalidOperationException>(scope, s => s.Commit(0));
        }
        Assert.Throws<InvalidOperationException>(() => default(WriteScope).Commit(0));
        default(WriteScope).Dispose();
    }

    [Fact]
    public void InvalidArguments()
    {
        foreach (string name in new[] { "", ".x", "a/b", "a b", "ä", new string('n', 65) })
        {
            foreach (Action call in new Action[]
            {
                () => StateReader.Open(name, Dir),
                () => StateWriter.Open(name, new StateOptions(8) { Directory = Dir }),
                () => Channel.Unlink(name, Dir),
            })
            {
                var e = Assert.Throws<PsMsgrException>(call);
                Assert.Equal((PsMsgrError.Inval, 0), (e.Code, e.Errno));
            }
        }
        StateReader.Open(new string('n', 64), Dir).Dispose();

        Assert.Throws<ArgumentException>(() => StateReader.Open("a\0b", Dir));
        Assert.Throws<ArgumentException>(() => Channel.Unlink(Chan, Dir + "\0x"));
        Assert.Throws<ArgumentException>(() => StateWriter.Open(Chan, new StateOptions(8) { Directory = Dir + "\0x" }));
        Assert.Throws<ArgumentNullException>(() => StateReader.Open(null!, Dir));
        Assert.Throws<ArgumentNullException>(() => StateWriter.Open(null!, new StateOptions(8)));
        Assert.Throws<ArgumentNullException>(() => StateWriter.Open(Chan, null!));

        foreach (var options in new[]
        {
            new StateOptions(8) { SlotCount = 1 },
            new StateOptions(8) { SlotCount = 17 },
            new StateOptions(8) { Mode = 0x11A4 }, // 010644
            new StateOptions((16u << 20) + 1),
        })
        {
            options.Directory = Dir;
            Assert.Equal(PsMsgrError.Inval, Assert.Throws<PsMsgrException>(() => StateWriter.Open(Chan, options)).Code);
        }
        Assert.False(File.Exists(LockPath())); // nothing created
    }

    [Fact]
    public void SystemErrors()
    {
        var e = Assert.Throws<PsMsgrException>(() => StateWriter.Open(Chan, new StateOptions(8) { Directory = Path.Combine(Dir, "missing") }));
        Assert.Equal((PsMsgrError.Sys, 2), (e.Code, e.Errno)); // ENOENT
        Assert.IsAssignableFrom<IOException>(e);
        Assert.Equal("No such file or directory (errno 2): 'chan'", e.Message);

        File.CreateSymbolicLink(DataPath(), Path.Combine(Dir, "elsewhere"));
        e = Assert.Throws<PsMsgrException>(() => OpenWriter(8));
        Assert.Equal((PsMsgrError.Sys, 40), (e.Code, e.Errno)); // ELOOP
        using var r = OpenReader();
        e = Assert.Throws<PsMsgrException>(() => r.TryPeek(out _));
        Assert.Equal((PsMsgrError.Sys, 40), (e.Code, e.Errno));
    }

    [Fact]
    public void FormatError()
    {
        File.WriteAllBytes(DataPath(), [.. Enumerable.Repeat(B("not a channel"), 20).SelectMany(b => b)]);
        Assert.Equal(PsMsgrError.Format, Assert.Throws<PsMsgrException>(() => OpenWriter(8)).Code);
        using (var r = OpenReader())
            Assert.Equal(PsMsgrError.Format, Assert.Throws<PsMsgrException>(() => r.Read(out _)).Code);
        OpenWriter(8, recreate: true).Dispose();
    }

    [Fact]
    public void Errors()
    {
        var e = new PsMsgrException(PsMsgrError.Busy, channelName: Chan);
        Assert.Equal("channel busy: 'chan'", e.Message);
        Assert.Equal(0, e.Errno);
        Assert.Equal("Permission denied (errno 13)", new PsMsgrException(PsMsgrError.Sys, errno: 13).Message);
        Assert.Equal("Unknown error 99999 (errno 99999)", new PsMsgrException(PsMsgrError.Sys, errno: 99999).Message);
        Assert.Equal("unknown error", new PsMsgrException((PsMsgrError)(-99)).Message);
        Assert.Equal("custom", new PsMsgrException(PsMsgrError.Inval, "custom").Message);
    }

    [Fact]
    public void UseAfterDispose()
    {
        var w = OpenWriter(8);
        var r = OpenReader();
        w.Dispose();
        w.Dispose();
        r.Dispose();
        r.Dispose();
        foreach (Action call in new Action[]
        {
            () => w.Publish([]),
            () => w.Publish(1),
            () => _ = w.Capacity,
            () => w.Begin(),
            () => r.Read(out _),
            () => r.TryRead(new byte[8], out _),
            () => r.TryRead(out int _, out _),
            () => r.TryPeek(out _),
            () => r.Wait(0, TimeSpan.Zero),
            () => _ = r.IsWriterAlive,
            () => r.Describe(),
        })
        {
            Assert.Throws<ObjectDisposedException>(call);
        }
    }
}
