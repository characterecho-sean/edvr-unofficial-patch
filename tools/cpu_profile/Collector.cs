using Microsoft.Diagnostics.Tracing;
using Microsoft.Diagnostics.Tracing.Etlx;
using Microsoft.Diagnostics.Tracing.Parsers.Kernel;

// TraceEvent discourages raw QPC access for ordinary analysis. EDVR markers deliberately
// carry raw QPC ticks, so exact cross-provider correlation requires the same clock domain.
#pragma warning disable CS0618

internal readonly record struct StackObs(long Qpc, int StackId);

internal readonly record struct ReadyObs(long Qpc, int WakerThread, int WakerProcess, int StackId);

internal sealed class CpuSwitchRange
{
    public long FirstQpc = long.MaxValue;
    public long LastQpc = long.MinValue;
    public long Count;
}

internal sealed class Collected
{
    public int EventsLost;
    public bool HasCallStacks;
    public long KernelFirstQpc = long.MaxValue;
    public long KernelLastQpc = long.MinValue;
    public int MalformedMarkers;
    public int UnsupportedMarkers;
    public int OtherMarkerEvents;
    public readonly List<string> MarkerErrors = [];
    public readonly List<ClockMarker> Clocks = [];
    public readonly List<SpanMarker> Spans = [];
    public readonly List<FrameMarker> Frames = [];
    public readonly Dictionary<int, ThreadTimeline> Timelines = [];
    public readonly Dictionary<string, long> RawSwitchStates = [];
    public readonly Dictionary<string, long> WaitReasons = [];
    public readonly Dictionary<int, CpuSwitchRange> CpuSwitchRanges = [];
    // Samples are kept for every thread of the process; switch-out stacks and
    // ready details only for caller threads, which is what bounds memory on a
    // four-minute file-mode trace.
    public readonly Dictionary<int, List<StackObs>> SamplesByThread = [];
    public readonly Dictionary<int, List<StackObs>> SwitchOutStacksByThread = [];
    public readonly Dictionary<int, List<ReadyObs>> ReadiesByThread = [];
    public readonly HashSet<int> CallerThreads = [];
    public long SwitchOut;
    public long SwitchIn;
    public long Ready;
    public long Samples;
    // A CSwitch payload carries thread ids, not process ids: TraceEvent resolves
    // the process from its own thread table, and the OLD thread of a switch can
    // be unresolved. An unrecorded switch-out leaves a Running state that never
    // closes, which reads as a thread pinned at 100% with no samples, so the
    // rescue below is counted rather than silent.
    public long SwitchOutUnresolvedProcess;
    public long SwitchInUnresolvedProcess;
    public long SwitchOutForeignProcessKnownThread;
    public readonly Dictionary<int, long> SwitchInByThread = [];
    public readonly Dictionary<int, long> SwitchOutByThread = [];
    public long SwitchOutBlockingStacks;
    public long SwitchOutBlockingStackOwnerMismatches;
    public long ReadyStacks;
    public long ReadyStackEmitterOwned;
    public long ReadyStackAwakenedOwned;
    public long ReadyStackUnownedOwner;
    public long SampleStacks;
    public long SampleStackOwnerMismatches;
    public long UnsortedTimelines;
    public double SampleIntervalUs;
    public string SampleIntervalSource = "";
    public double SampleIntervalFromLogUs;
    public double SampleIntervalFromEventsUs;
    public StackStore Stacks = null!;

    public ThreadTimeline Timeline(int thread)
    {
        if (!Timelines.TryGetValue(thread, out var timeline)) Timelines[thread] = timeline = new ThreadTimeline();
        return timeline;
    }

    public static void Append<T>(Dictionary<int, List<T>> map, int key, T value)
    {
        if (!map.TryGetValue(key, out var list)) map[key] = list = [];
        list.Add(value);
    }
}

internal static class Collector
{
    public static readonly Guid EdvrProvider = new("D3885FA1-0B70-44F1-AF88-63B2012B111E");

    public static Collected Collect(TraceLog log, int pid, Action<string>? progress = null)
    {
        var result = new Collected { EventsLost = log.EventsLost, HasCallStacks = log.HasCallStacks };
        // Pass 1 decodes only EDVR markers. The caller thread is not known before
        // the first completed-frame marker, and it decides whose switch-out stacks
        // are worth keeping in pass 2.
        foreach (var data in log.Events)
            if (data.ProviderGuid == EdvrProvider && data.ProcessID == pid)
                Markers.Parse(data, result);
        foreach (var frame in result.Frames)
        {
            if (frame.CallerThread != 0) result.CallerThreads.Add(checked((int)frame.CallerThread));
            if (frame.NextWaitThread != 0) result.CallerThreads.Add(checked((int)frame.NextWaitThread));
        }
        progress?.Invoke($"markers: clocks={result.Clocks.Count} spans={result.Spans.Count} " +
                         $"frames={result.Frames.Count} callerThreads={result.CallerThreads.Count}");

        result.Stacks = new StackStore(log, pid);
        result.SampleIntervalFromLogUs = log.SampleProfileInterval.TotalMilliseconds * 1000.0;
        var processThreads = log.Threads.Where(thread => thread.Process.ProcessID == pid)
            .Select(thread => thread.ThreadID).ToHashSet();
        foreach (var data in log.Events)
        {
            switch (data)
            {
                case CSwitchTraceData cs:
                    NoteSwitch(result, data.ProcessorNumber, data.TimeStampQPC);
                    var oldIsOurs = cs.OldProcessID == pid;
                    if (!oldIsOurs && processThreads.Contains(cs.OldThreadID))
                    {
                        if (cs.OldProcessID <= 0) { oldIsOurs = true; result.SwitchOutUnresolvedProcess++; }
                        else result.SwitchOutForeignProcessKnownThread++;
                    }
                    if (oldIsOurs)
                    {
                        result.SwitchOut++;
                        Numeric.Increment(result.SwitchOutByThread, cs.OldThreadID);
                        Numeric.Increment(result.RawSwitchStates, cs.OldThreadState.ToString());
                        Numeric.Increment(result.WaitReasons, cs.OldThreadWaitReason.ToString());
                        result.Timeline(cs.OldThreadID).Add(data.TimeStampQPC, ClassifyOldState(cs.OldThreadState));
                        if (result.CallerThreads.Contains(cs.OldThreadID)) AddBlockingStack(log, cs, result);
                    }
                    var newIsOurs = cs.NewProcessID == pid;
                    if (!newIsOurs && cs.NewProcessID <= 0 && processThreads.Contains(cs.NewThreadID))
                    {
                        newIsOurs = true;
                        result.SwitchInUnresolvedProcess++;
                    }
                    if (newIsOurs)
                    {
                        result.SwitchIn++;
                        Numeric.Increment(result.SwitchInByThread, cs.NewThreadID);
                        result.Timeline(cs.NewThreadID).Add(data.TimeStampQPC, CpuState.Running);
                    }
                    break;
                case DispatcherReadyThreadTraceData ready when ready.AwakenedProcessID == pid:
                    NoteKernel(result, data.TimeStampQPC);
                    result.Ready++;
                    result.Timeline(ready.AwakenedThreadID).Add(data.TimeStampQPC, CpuState.Ready);
                    if (result.CallerThreads.Contains(ready.AwakenedThreadID)) AddReady(log, ready, result);
                    break;
                case SampledProfileTraceData sample when sample.ProcessID == pid:
                    NoteKernel(result, data.TimeStampQPC);
                    result.Samples++;
                    AddSample(log, sample, result);
                    break;
                case SampledProfileIntervalTraceData interval when interval.NewInterval > 0:
                    result.SampleIntervalFromEventsUs = interval.NewInterval / 10.0;
                    break;
            }
        }
        foreach (var timeline in result.Timelines.Values)
            if (!timeline.Sorted()) result.UnsortedTimelines++;
        result.SampleIntervalUs = result.SampleIntervalFromEventsUs > 0 ? result.SampleIntervalFromEventsUs
            : result.SampleIntervalFromLogUs > 0 ? result.SampleIntervalFromLogUs : 1000.0;
        result.SampleIntervalSource = result.SampleIntervalFromEventsUs > 0
            ? "SampledProfileInterval event (NewInterval)"
            : result.SampleIntervalFromLogUs > 0 ? "TraceLog.SampleProfileInterval"
            : "assumed 1000us: the trace carries no sampling-interval record";
        progress?.Invoke($"scheduler: switchOut={result.SwitchOut} switchIn={result.SwitchIn} " +
                         $"ready={result.Ready} samples={result.Samples} stacks={result.Stacks.Count}");
        return result;
    }

    private static void NoteKernel(Collected data, long qpc)
    {
        data.KernelFirstQpc = Math.Min(data.KernelFirstQpc, qpc);
        data.KernelLastQpc = Math.Max(data.KernelLastQpc, qpc);
    }

    private static void NoteSwitch(Collected data, int processor, long qpc)
    {
        NoteKernel(data, qpc);
        if (!data.CpuSwitchRanges.TryGetValue(processor, out var range))
            data.CpuSwitchRanges[processor] = range = new CpuSwitchRange();
        range.FirstQpc = Math.Min(range.FirstQpc, qpc);
        range.LastQpc = Math.Max(range.LastQpc, qpc);
        range.Count++;
    }

    public static CpuState ClassifyOldState(System.Diagnostics.ThreadState state) =>
        ClassifyOldState((int)state);

    public static CpuState ClassifyOldState(int rawState) => rawState switch
    {
        // ETW CSwitch state 2 remains Running across observed CPU handoffs. State 7 is
        // DeferredReady, although System.Diagnostics.ThreadState exposes value 7 as Unknown.
        // https://learn.microsoft.com/en-us/windows/win32/etw/cswitch
        1 or 3 or 7 => CpuState.Ready,
        2 => CpuState.Running,
        5 or 6 => CpuState.Waiting,
        _ => CpuState.UnexpectedUnknown,
    };

    private static void AddBlockingStack(TraceLog log, CSwitchTraceData data, Collected result)
    {
        // TraceEvent associates the ordinary event stack with the thread that GOT the CPU.
        // BlockingStack is the distinct stack for the old thread that LOST the CPU.
        var index = data.BlockingStack();
        if (index == CallStackIndex.Invalid) return;
        result.SwitchOutBlockingStacks++;
        var owner = log.CallStacks.Thread(index);
        if (owner is null || !StackOwnerMatches(owner.Process.ProcessID, owner.ThreadID,
                                                data.OldProcessID, data.OldThreadID))
        {
            result.SwitchOutBlockingStackOwnerMismatches++;
            return;
        }
        Collected.Append(result.SwitchOutStacksByThread, data.OldThreadID,
                         new StackObs(data.TimeStampQPC, result.Stacks.Intern(index, data.TimeStampRelativeMSec)));
    }

    public static bool StackOwnerMatches(int ownerProcess, int ownerThread,
                                         int expectedProcess, int expectedThread) =>
        ownerProcess == expectedProcess && ownerThread == expectedThread;

    private static void AddReady(TraceLog log, DispatcherReadyThreadTraceData data, Collected result)
    {
        // A ReadyThread event is emitted by the thread that made another thread
        // runnable, so its stack belongs to the waker, not to the awakened thread.
        // The owner counters below prove that against this trace rather than
        // assuming it.
        var index = log.GetCallStackIndexForEvent(data);
        var stackId = -1;
        if (index != CallStackIndex.Invalid)
        {
            result.ReadyStacks++;
            var owner = log.CallStacks.Thread(index);
            if (owner is null) result.ReadyStackUnownedOwner++;
            else if (owner.ThreadID == data.ThreadID) result.ReadyStackEmitterOwned++;
            else if (owner.ThreadID == data.AwakenedThreadID) result.ReadyStackAwakenedOwned++;
            else result.ReadyStackUnownedOwner++;
            stackId = result.Stacks.Intern(index, data.TimeStampRelativeMSec);
        }
        Collected.Append(result.ReadiesByThread, data.AwakenedThreadID,
                         new ReadyObs(data.TimeStampQPC, data.ThreadID, data.ProcessID, stackId));
    }

    private static void AddSample(TraceLog log, SampledProfileTraceData data, Collected result)
    {
        var index = log.GetCallStackIndexForEvent(data);
        if (index == CallStackIndex.Invalid) return;
        result.SampleStacks++;
        var owner = log.CallStacks.Thread(index);
        if (owner is not null && owner.ThreadID != data.ThreadID)
        {
            result.SampleStackOwnerMismatches++;
            return;
        }
        Collected.Append(result.SamplesByThread, data.ThreadID,
                         new StackObs(data.TimeStampQPC, result.Stacks.Intern(index, data.TimeStampRelativeMSec)));
    }
}
