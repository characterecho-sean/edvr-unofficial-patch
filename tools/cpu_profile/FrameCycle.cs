// One frame cycle, reconstructed from the completed-frame marker plus the two
// Submit spans on the caller thread, then cut into the regions
// src\openxr\frame_cycle_stats.h defines:
//
//   waitReturn -> firstSubmitEntry        R1  game_before_first_submit
//   firstSubmitEntry -> firstSubmitReturn R2  first_submit_roundtrip
//   firstSubmitReturn -> secondSubmitEntry R3 between_eye_calls
//   secondSubmitEntry -> secondSubmitReturn R4 second_submit_roundtrip
//   secondSubmitReturn -> nextWaitEntry   R5a+R5b+R5c post_second_submit_to_next_wait
//   nextWaitEntry -> nextWaitReturn       R6  next_wait_roundtrip
//
// The frame marker carries no first-Submit timestamps, so R1..R4 are split by
// the op-2 span markers. A span begins before the runtime takes its own tick and
// ends after it, so every derived boundary carries a measured offset instead of
// an assumption: see SubmitSpanEndOffsetUs, which is exactly
// (second Submit span end - the marker's secondSubmitReturn).

internal sealed class RegionResult
{
    public string Name = "";
    public double StartUs;
    public double EndUs;
    public StateDurations States;
    public double LengthUs => EndUs - StartUs;
    public double ResidualUs => LengthUs - States.Total;
}

internal sealed class WaitSegment
{
    public double StartUs;
    public double EndUs;
    public double InsideUs;
    public bool EndedReady;
    public int BlockingStackId = -1;
    public GameTop BlockingTops;
    public int WakerThread;
    public int WakerProcess;
    public int WakerReadyStackId = -1;
    public GameTop WakerReadyTops;
    public string WakerClass = "";
    public double WakerSampleAgeUs;
    public bool WakerSampleFound;
    public double LengthUs => EndUs - StartUs;
}

internal sealed class ThreadBusy
{
    public int ThreadId;
    public double RunningUs;
    public int Samples;
    public int PipelineSamples;
    public int[]? ClassSamples;
}

internal sealed class FrameCycle
{
    public FrameMarker Marker = null!;
    public ulong WaitReturnUs, FirstSubmitEntryUs, FirstSubmitReturnUs, SecondSubmitEntryUs;
    public ulong SecondSubmitReturnUs, PresentBeginUs, PresentEndUs, NextWaitEntryUs, NextWaitReturnUs;
    public DateTime WaitReturnUtc;
    public bool Derived;
    public string DerivationReason = "";
    public double CycleMs, BeforeFirstMs, FirstSubmitMs, BetweenMs, SecondSubmitMs, PostSubmitMs, NextWaitMs;
    public double ResidualMs;
    public bool SubmitOffsetKnown;
    public double SubmitSpanEndOffsetUs;
    public bool WaitOffsetKnown;
    public double WaitSpanBeginOffsetUs;
    public double WaitSpanEndOffsetUs;
    public bool Covered;
    public bool LegacyCovered;
    public string? LegacyDiscardReason;
    public RegionResult? LegacyRegion;
    public RegionResult[] Regions = [];
    public List<WaitSegment> Waits = [];
    public List<ThreadBusy> Threads = [];
    public int[] CallerClassSamples = [];
    public int CallerSamples;
    public int CallerPipelineSamples;
    public int CallerSwitchOutStacks;
    public int CallerReadyStacks;
    public double CallerPipelineRunningUs;
    public double PipelineWaitUs;
    public double ThreadPipelineRunningUs;
    public double CriticalPathShare;
    public int WindowNumber = -1;

    public double PhaseMs(string name) => name switch
    {
        "cycle" => CycleMs,
        "game_before_first_submit" => BeforeFirstMs,
        "first_submit_roundtrip" => FirstSubmitMs,
        "between_eye_calls" => BetweenMs,
        "second_submit_roundtrip" => SecondSubmitMs,
        "post_second_submit_to_next_wait" => PostSubmitMs,
        "next_wait_roundtrip" => NextWaitMs,
        "per_frame_residual" => ResidualMs,
        _ => double.NaN,
    };

    public RegionResult? Region(string name) => Regions.FirstOrDefault(region => region.Name == name);
}

internal sealed class CycleAnalyzer
{
    public const double StaleWakerSampleUs = 2000.0;
    public static readonly string[] RegionNames = ["R1", "R2", "R3", "R4", "R5a", "R5b", "R5c", "R6", "C1", "C2", "C3"];

    private readonly Collected _data;
    private readonly Func<long, double> _qpcUs;
    private readonly double _traceStartUs;
    private readonly double _traceEndUs;
    private readonly ClockMarker[] _clocks;
    private readonly Dictionary<uint, (ulong Begin, ulong End)[]> _submitSpans = [];
    private readonly Dictionary<uint, (ulong Begin, ulong End)[]> _waitSpans = [];
    private readonly Dictionary<int, (double Start, double End, bool Ready)[]> _waitSegments = [];
    private readonly int[] _threadIds;
    private readonly double _intervalUs;
    private readonly int _pid;

    public readonly long[] ClassSampleTotals = new long[RvaTable.Classes.Length];
    public readonly ulong[] ClassFirstSequence = new ulong[RvaTable.Classes.Length];
    public readonly ulong[] ClassLastSequence = new ulong[RvaTable.Classes.Length];
    public readonly Dictionary<int, long[]> ClassSamplesByThread = [];
    public long DerivedFrames;
    public long CoveredWithoutDerivation;
    public readonly Dictionary<string, long> DerivationFailures = [];

    public CycleAnalyzer(Collected data, int pid, Func<long, double> qpcUs, double traceStartUs, double traceEndUs)
    {
        _data = data;
        _pid = pid;
        _qpcUs = qpcUs;
        _traceStartUs = traceStartUs;
        _traceEndUs = traceEndUs;
        _clocks = data.Clocks.OrderBy(clock => clock.TimestampUs).ToArray();
        _intervalUs = data.SampleIntervalUs;
        _threadIds = data.Timelines.Keys.OrderBy(id => id).ToArray();
        foreach (var group in data.Spans.Where(span => span.TimestampUs >= span.BeginUs).GroupBy(span => span.Operation))
        {
            if (group.Key is not (Markers.SubmitOperation or Markers.WaitOperation)) continue;
            var target = group.Key == Markers.SubmitOperation ? _submitSpans : _waitSpans;
            foreach (var byThread in group.GroupBy(span => span.Thread))
                target[byThread.Key] = byThread.OrderBy(span => span.BeginUs)
                    .Select(span => (span.BeginUs, span.TimestampUs)).ToArray();
        }
        foreach (var thread in data.CallerThreads) _waitSegments[thread] = WaitSegmentsOf(thread);
    }

    private (double Start, double End, bool Ready)[] WaitSegmentsOf(int thread)
    {
        if (!_data.Timelines.TryGetValue(thread, out var timeline)) return [];
        var segments = new List<(double, double, bool)>();
        for (var i = 0; i < timeline.Count - 1; i++)
        {
            if (timeline.StateAt(i) != CpuState.Waiting) continue;
            segments.Add((_qpcUs(timeline.QpcAt(i)), _qpcUs(timeline.QpcAt(i + 1)),
                          timeline.StateAt(i + 1) == CpuState.Ready));
        }
        return segments.ToArray();
    }

    public FrameCycle Analyze(FrameMarker marker)
    {
        var cycle = new FrameCycle
        {
            Marker = marker,
            WaitReturnUs = marker.WaitReturnUs,
            SecondSubmitReturnUs = marker.SecondSubmitReturnUs,
            PresentBeginUs = marker.PresentBeginUs,
            PresentEndUs = marker.PresentEndUs,
            NextWaitEntryUs = marker.NextWaitEntryUs,
            NextWaitReturnUs = marker.NextWaitReturnUs,
            WaitReturnUtc = Markers.Utc(_clocks, marker.WaitReturnUs),
        };
        cycle.CycleMs = Ms(marker.NextWaitReturnUs, marker.WaitReturnUs);
        cycle.PostSubmitMs = Ms(marker.NextWaitEntryUs, marker.SecondSubmitReturnUs);
        cycle.NextWaitMs = Ms(marker.NextWaitReturnUs, marker.NextWaitEntryUs);
        DeriveSubmits(cycle);
        MeasureWaitSpan(cycle);
        if (cycle.Derived) DerivedFrames++;
        else Numeric.Increment(DerivationFailures, cycle.DerivationReason);

        var caller = (int)marker.CallerThread;
        var timeline = _data.Timelines.GetValueOrDefault(caller);

        // The legacy [presentEnd, nextWaitEntry) window keeps its own admission
        // rule, so report.json's analyzedFrameCount and frames array keep the
        // exact meaning earlier sessions and cpu_profile.py's smoke check read.
        var legacyStart = (double)marker.PresentEndUs;
        var legacyEnd = (double)marker.NextWaitEntryUs;
        cycle.LegacyDiscardReason =
            legacyEnd <= _traceStartUs ? "prefix" :
            legacyStart >= _traceEndUs ? "suffix" :
            legacyStart < _traceStartUs || legacyEnd > _traceEndUs ? "partial" : null;
        if (cycle.LegacyDiscardReason is null)
        {
            cycle.LegacyRegion = new RegionResult
            {
                Name = "R5c",
                StartUs = legacyStart,
                EndUs = legacyEnd,
                States = Numeric.Durations(timeline, legacyStart, legacyEnd, _qpcUs),
            };
            cycle.LegacyCovered = cycle.LegacyRegion.States.BoundaryUnknown <= 0.001;
            if (!cycle.LegacyCovered) cycle.LegacyDiscardReason = "boundary_unknown";
        }

        var inCoverage = marker.WaitReturnUs >= _traceStartUs && marker.NextWaitReturnUs <= _traceEndUs &&
                         timeline is not null;
        if (inCoverage && !cycle.Derived) CoveredWithoutDerivation++;
        cycle.Covered = inCoverage && cycle.Derived;
        if (!cycle.Covered) return cycle;

        cycle.Regions = BuildRegions(cycle, timeline!);
        CollectThreads(cycle, caller);
        CollectWaits(cycle, caller);
        cycle.CallerPipelineRunningUs = cycle.CallerPipelineSamples * _intervalUs;
        cycle.CriticalPathShare = cycle.ThreadPipelineRunningUs > 0
            ? (cycle.CallerPipelineRunningUs + cycle.PipelineWaitUs) / cycle.ThreadPipelineRunningUs : 0;
        return cycle;
    }

    private static double Ms(ulong end, ulong start) => ((double)end - start) / 1000.0;

    private void DeriveSubmits(FrameCycle cycle)
    {
        cycle.DerivationReason = "";
        if (!_submitSpans.TryGetValue(cycle.Marker.CallerThread, out var spans))
        {
            cycle.DerivationReason = "no_submit_spans_on_caller_thread";
            return;
        }
        // The two Submit spans of this frame are the op-2 spans that BEGIN inside
        // [waitReturn, secondSubmitReturn). The second span's end lies after the
        // marker's secondSubmitReturn, so it must not be used as the filter.
        var low = Numeric.LowerBound(spans.Length, index => spans[index].Begin >= cycle.Marker.WaitReturnUs);
        var high = Numeric.LowerBound(spans.Length, index => spans[index].Begin >= cycle.Marker.SecondSubmitReturnUs);
        var count = high - low;
        if (count != 2)
        {
            cycle.DerivationReason = $"submit_span_count_{Math.Min(count, 9)}";
            return;
        }
        var first = spans[low];
        var second = spans[low + 1];
        if (first.End > second.Begin) { cycle.DerivationReason = "submit_spans_overlap"; return; }
        if (second.End < cycle.Marker.SecondSubmitReturnUs)
        {
            cycle.DerivationReason = "second_submit_span_ends_before_marker";
            return;
        }
        cycle.FirstSubmitEntryUs = first.Begin;
        cycle.FirstSubmitReturnUs = first.End;
        cycle.SecondSubmitEntryUs = second.Begin;
        cycle.SubmitOffsetKnown = true;
        cycle.SubmitSpanEndOffsetUs = (double)second.End - cycle.Marker.SecondSubmitReturnUs;
        cycle.BeforeFirstMs = Ms(first.Begin, cycle.Marker.WaitReturnUs);
        cycle.FirstSubmitMs = Ms(first.End, first.Begin);
        cycle.BetweenMs = Ms(second.Begin, first.End);
        cycle.SecondSubmitMs = Ms(cycle.Marker.SecondSubmitReturnUs, second.Begin);
        cycle.ResidualMs = cycle.CycleMs - (cycle.BeforeFirstMs + cycle.FirstSubmitMs + cycle.BetweenMs +
                                            cycle.SecondSubmitMs + cycle.PostSubmitMs + cycle.NextWaitMs);
        cycle.Derived = true;
    }

    private void MeasureWaitSpan(FrameCycle cycle)
    {
        if (!_waitSpans.TryGetValue(cycle.Marker.CallerThread, out var spans)) return;
        var index = Numeric.LowerBound(spans.Length, i => spans[i].Begin > cycle.Marker.NextWaitEntryUs) - 1;
        if (index < 0) return;
        var span = spans[index];
        if (span.End < cycle.Marker.NextWaitReturnUs) return;
        cycle.WaitOffsetKnown = true;
        cycle.WaitSpanBeginOffsetUs = (double)cycle.Marker.NextWaitEntryUs - span.Begin;
        cycle.WaitSpanEndOffsetUs = (double)span.End - cycle.Marker.NextWaitReturnUs;
    }

    private RegionResult[] BuildRegions(FrameCycle cycle, ThreadTimeline timeline)
    {
        (string Name, double Start, double End)[] bounds =
        [
            ("R1", cycle.WaitReturnUs, cycle.FirstSubmitEntryUs),
            ("R2", cycle.FirstSubmitEntryUs, cycle.FirstSubmitReturnUs),
            ("R3", cycle.FirstSubmitReturnUs, cycle.SecondSubmitEntryUs),
            ("R4", cycle.SecondSubmitEntryUs, cycle.SecondSubmitReturnUs),
            ("R5a", cycle.SecondSubmitReturnUs, cycle.PresentBeginUs),
            ("R5b", cycle.PresentBeginUs, cycle.PresentEndUs),
            ("R5c", cycle.PresentEndUs, cycle.NextWaitEntryUs),
            ("R6", cycle.NextWaitEntryUs, cycle.NextWaitReturnUs),
            ("C1", cycle.WaitReturnUs, cycle.FirstSubmitEntryUs),
            ("C2", cycle.FirstSubmitEntryUs, cycle.PresentEndUs),
            ("C3", cycle.PresentEndUs, cycle.NextWaitEntryUs),
        ];
        var regions = new RegionResult[bounds.Length];
        for (var i = 0; i < bounds.Length; i++)
            regions[i] = new RegionResult
            {
                Name = bounds[i].Name,
                StartUs = bounds[i].Start,
                EndUs = bounds[i].End,
                States = Numeric.Durations(timeline, bounds[i].Start, bounds[i].End, _qpcUs),
            };
        return regions;
    }

    private void CollectThreads(FrameCycle cycle, int caller)
    {
        var start = (double)cycle.WaitReturnUs;
        var end = (double)cycle.NextWaitReturnUs;
        foreach (var thread in _threadIds)
        {
            var running = Numeric.RunningUs(_data.Timelines[thread], start, end, _qpcUs);
            var busy = new ThreadBusy { ThreadId = thread, RunningUs = running };
            CountSamples(busy, thread, start, end, cycle.Marker.Sequence);
            cycle.ThreadPipelineRunningUs += busy.PipelineSamples * _intervalUs;
            if (thread == caller)
            {
                cycle.CallerSamples = busy.Samples;
                cycle.CallerPipelineSamples = busy.PipelineSamples;
                cycle.CallerClassSamples = busy.ClassSamples ?? new int[RvaTable.Classes.Length];
            }
            if (running > 0 || busy.Samples > 0 || thread == caller) cycle.Threads.Add(busy);
        }
    }

    private void CountSamples(ThreadBusy busy, int thread, double start, double end, ulong sequence)
    {
        if (!_data.SamplesByThread.TryGetValue(thread, out var samples)) return;
        var low = Numeric.LowerBound(samples.Count, index => _qpcUs(samples[index].Qpc) >= start);
        for (var i = low; i < samples.Count && _qpcUs(samples[i].Qpc) < end; i++)
        {
            var stack = _data.Stacks[samples[i].StackId];
            busy.Samples++;
            if (stack.Pipeline) busy.PipelineSamples++;
            foreach (var classId in stack.ClassIds)
            {
                busy.ClassSamples ??= new int[RvaTable.Classes.Length];
                busy.ClassSamples[classId]++;
                ClassSampleTotals[classId]++;
                if (ClassFirstSequence[classId] == 0) ClassFirstSequence[classId] = sequence;
                ClassLastSequence[classId] = sequence;
                if (!ClassSamplesByThread.TryGetValue(thread, out var perThread))
                    ClassSamplesByThread[thread] = perThread = new long[RvaTable.Classes.Length];
                perThread[classId]++;
            }
        }
    }

    private void CollectWaits(FrameCycle cycle, int caller)
    {
        if (!_waitSegments.TryGetValue(caller, out var segments)) return;
        var start = (double)cycle.WaitReturnUs;
        var end = (double)cycle.NextWaitReturnUs;
        var low = Numeric.LowerBound(segments.Length, index => segments[index].Start >= start);
        for (var i = low; i < segments.Length && segments[i].Start < end; i++)
        {
            var raw = segments[i];
            var segment = new WaitSegment
            {
                StartUs = raw.Start,
                EndUs = raw.End,
                EndedReady = raw.Ready,
                InsideUs = Math.Max(0, Math.Min(raw.End, end) - raw.Start),
            };
            Attribute(segment, caller);
            cycle.Waits.Add(segment);
            if (segment.WakerClass == "pipeline") cycle.PipelineWaitUs += segment.InsideUs;
        }
        cycle.CallerSwitchOutStacks = CountObs(_data.SwitchOutStacksByThread, caller, start, end);
        cycle.CallerReadyStacks = CountReady(caller, start, end);
    }

    private int CountObs(Dictionary<int, List<StackObs>> map, int thread, double start, double end)
    {
        if (!map.TryGetValue(thread, out var list)) return 0;
        var low = Numeric.LowerBound(list.Count, index => _qpcUs(list[index].Qpc) >= start);
        var count = 0;
        for (var i = low; i < list.Count && _qpcUs(list[i].Qpc) < end; i++) count++;
        return count;
    }

    private int CountReady(int thread, double start, double end)
    {
        if (!_data.ReadiesByThread.TryGetValue(thread, out var list)) return 0;
        var low = Numeric.LowerBound(list.Count, index => _qpcUs(list[index].Qpc) >= start);
        var count = 0;
        for (var i = low; i < list.Count && _qpcUs(list[i].Qpc) < end; i++)
            if (list[i].StackId >= 0) count++;
        return count;
    }

    private void Attribute(WaitSegment segment, int caller)
    {
        if (_data.SwitchOutStacksByThread.TryGetValue(caller, out var switchOuts))
        {
            var index = Numeric.LowerBound(switchOuts.Count, i => _qpcUs(switchOuts[i].Qpc) >= segment.StartUs);
            if (index < switchOuts.Count && Math.Abs(_qpcUs(switchOuts[index].Qpc) - segment.StartUs) < 0.001)
            {
                segment.BlockingStackId = switchOuts[index].StackId;
                segment.BlockingTops = _data.Stacks[segment.BlockingStackId].GameTops;
            }
        }
        if (!_data.ReadiesByThread.TryGetValue(caller, out var readies))
        {
            segment.WakerClass = "no_ready_event";
            return;
        }
        var readyIndex = Numeric.LowerBound(readies.Count, i => _qpcUs(readies[i].Qpc) >= segment.EndUs);
        if (readyIndex >= readies.Count || Math.Abs(_qpcUs(readies[readyIndex].Qpc) - segment.EndUs) > 0.001)
        {
            segment.WakerClass = "no_ready_event";
            return;
        }
        var ready = readies[readyIndex];
        segment.WakerThread = ready.WakerThread;
        segment.WakerProcess = ready.WakerProcess;
        segment.WakerReadyStackId = ready.StackId;
        if (ready.StackId >= 0) segment.WakerReadyTops = _data.Stacks[ready.StackId].GameTops;
        segment.WakerClass = WakerClass(segment, ready);
    }

    /// What was signalled, not who signalled it: a worker thread's ready call sits
    /// in the synchronisation primitive it released, so the ready stack alone
    /// cannot say which job just finished. The waker's most recent sampled stack
    /// can.
    private string WakerClass(WaitSegment segment, ReadyObs ready)
    {
        if (ready.WakerThread <= 0) return "idle_or_dpc";
        if (ready.WakerProcess != _pid) return "other_process";
        if (!_data.SamplesByThread.TryGetValue(ready.WakerThread, out var samples)) return "no_sample";
        var index = Numeric.LowerBound(samples.Count, i => _qpcUs(samples[i].Qpc) >= segment.EndUs) - 1;
        if (index < 0) return "no_sample";
        var age = segment.EndUs - _qpcUs(samples[index].Qpc);
        segment.WakerSampleAgeUs = age;
        segment.WakerSampleFound = true;
        if (age > StaleWakerSampleUs) return "stale_sample";
        return LabelName(_data.Stacks[samples[index].StackId].Label);
    }

    public static string LabelName(StackLabel label) => label switch
    {
        StackLabel.Pipeline => "pipeline",
        StackLabel.Scheduler => "scheduler",
        StackLabel.GameOther => "game_other",
        StackLabel.EdvrD3d11 => "edvr_d3d11",
        StackLabel.EdvrOpenvrApi => "edvr_openvr_api",
        StackLabel.OtherModule => "other_module",
        StackLabel.KernelOnly => "kernel_only",
        _ => "empty",
    };
}
