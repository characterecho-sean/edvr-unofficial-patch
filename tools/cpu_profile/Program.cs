using System.Buffers.Binary;
using System.Text.Json;
using Microsoft.Diagnostics.Tracing;
using Microsoft.Diagnostics.Tracing.Etlx;
using Microsoft.Diagnostics.Tracing.Parsers.Kernel;

// TraceEvent discourages raw QPC access for ordinary analysis. EDVR markers deliberately
// carry raw QPC ticks, so exact cross-provider correlation requires the same clock domain.
#pragma warning disable CS0618

internal static class Program
{
    private static readonly Guid EdvrProvider = new("D3885FA1-0B70-44F1-AF88-63B2012B111E");
    private const ushort MarkerVersion = 1;
    private const ushort PostValid = 1 << 0;
    private const ushort SinglePresent = 1 << 1;

    private enum CpuState { Unknown, Running, Ready, Waiting }
    private sealed record Transition(long Qpc, CpuState State, string Detail);
    private sealed record ClockMarker(ulong TimestampUs, ulong QpcTicks, ulong QpcFrequency,
                                      ulong SystemTime100ns, long HeaderQpc);
    private sealed record SpanMarker(ulong TimestampUs, ulong CallId, ulong BeginUs, long Result,
                                     uint Thread, ushort Operation, ushort Flags);
    private sealed record FrameMarker(ulong TimestampUs, ulong Sequence, ulong Generation,
                                      ulong FeatureEpoch, ulong WaitReturnUs, ulong SecondSubmitReturnUs,
                                      ulong NextWaitEntryUs, ulong NextWaitReturnUs, ulong PresentBeginUs,
                                      ulong PresentEndUs, uint CallerThread, uint NextWaitThread,
                                      ushort Status, ushort Flags, uint SceneReady);
    private sealed record StackObservation(long Qpc, int Thread, string Kind, string[] Frames);

    private sealed class SpanCursor
    {
        private readonly SpanMarker[] _spans;
        private readonly List<SpanMarker> _active = [];
        private int _next;
        private ulong _lastStart;

        public SpanCursor(IEnumerable<SpanMarker> spans) =>
            _spans = spans.OrderBy(span => span.BeginUs).ToArray();

        public SpanMarker[] Query(ulong start, ulong end)
        {
            if (start < _lastStart)
            {
                _active.Clear();
                _next = 0;
            }
            _lastStart = start;
            _active.RemoveAll(span => span.TimestampUs <= start);
            while (_next < _spans.Length && _spans[_next].BeginUs < end)
            {
                var span = _spans[_next++];
                if (span.TimestampUs > start) _active.Add(span);
            }
            return _active.Where(span => span.BeginUs < end && span.TimestampUs > start).ToArray();
        }
    }

    private sealed class Collected
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
        public readonly Dictionary<int, List<Transition>> Timelines = [];
        public readonly List<StackObservation> Stacks = [];
        public readonly Dictionary<string, long> RawSwitchStates = [];
        public readonly Dictionary<string, long> WaitReasons = [];
        public long SwitchOut;
        public long SwitchIn;
        public long Ready;
        public long Samples;
    }

    public static int Main(string[] args)
    {
        try
        {
            if (args.Length == 1 && args[0] == "--self-test")
            {
                SelfTest();
                Console.WriteLine("EdvrCpuProfile self-test passed");
                return 0;
            }

            var options = ParseOptions(args);
            var report = Analyze(options.Input, options.Pid);
            var jsonOptions = new JsonSerializerOptions { WriteIndented = true };
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(options.Output))!);
            File.WriteAllText(options.Output, JsonSerializer.Serialize(report, jsonOptions));
            Console.WriteLine($"EDVR CPU report: {options.Output}");
            Console.WriteLine($"frames={report["frameCount"]} analyzed={report["analyzedFrameCount"]} " +
                              $"eventsLost={report["eventsLost"]} coverageComplete={report["coverageComplete"]}");
            return (bool)report["coverageComplete"]! ? 0 : 2;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"EdvrCpuProfile: {ex.Message}");
            return 1;
        }
    }

    private sealed record Options(string Input, int Pid, string Output);

    private static Options ParseOptions(string[] args)
    {
        string? input = null, output = null;
        int? pid = null;
        for (var i = 0; i < args.Length; i++)
        {
            if (args[i] == "--input" && ++i < args.Length) input = args[i];
            else if (args[i] == "--pid" && ++i < args.Length && int.TryParse(args[i], out var parsed)) pid = parsed;
            else if (args[i] == "--output" && ++i < args.Length) output = args[i];
            else throw new ArgumentException("usage: EdvrCpuProfile --input <etl> --pid <pid> --output <report.json> | --self-test");
        }
        if (string.IsNullOrWhiteSpace(input) || string.IsNullOrWhiteSpace(output) || pid is null || pid <= 0)
            throw new ArgumentException("usage: EdvrCpuProfile --input <etl> --pid <pid> --output <report.json> | --self-test");
        if (!File.Exists(input)) throw new FileNotFoundException("ETL input does not exist", input);
        return new Options(Path.GetFullPath(input), pid.Value, Path.GetFullPath(output));
    }

    private static Dictionary<string, object?> Analyze(string input, int pid)
    {
        var tempEtlx = Path.Combine(Path.GetTempPath(), $"edvr-cpu-{Guid.NewGuid():N}.etlx");
        try
        {
            var converted = TraceLog.CreateFromEventTraceLogFile(input, tempEtlx, new TraceLogOptions(),
                                                                  new TraceEventDispatcherOptions());
            using var log = new TraceLog(converted);
            var data = Collect(log, pid);
            return BuildReport(input, pid, data);
        }
        finally
        {
            try { if (File.Exists(tempEtlx)) File.Delete(tempEtlx); }
            catch { /* A report is more useful than failing because a temporary ETLX stayed open. */ }
        }
    }

    private static Collected Collect(TraceLog log, int pid)
    {
        var result = new Collected { EventsLost = log.EventsLost, HasCallStacks = log.HasCallStacks };
        foreach (var data in log.Events)
        {
            if (data.ProviderGuid == EdvrProvider && data.ProcessID == pid)
            {
                ParseMarker(data, result);
                continue;
            }

            if (data is CSwitchTraceData cs)
            {
                NoteKernel(result, data.TimeStampQPC);
                if (cs.OldProcessID == pid)
                {
                    result.SwitchOut++;
                    Increment(result.RawSwitchStates, cs.OldThreadState.ToString());
                    Increment(result.WaitReasons, cs.OldThreadWaitReason.ToString());
                    AddTransition(result, cs.OldThreadID, data.TimeStampQPC,
                                  ClassifyOldState(cs.OldThreadState), cs.OldThreadWaitReason.ToString());
                    AddStack(log, data, cs.OldThreadID, "switch-out", result);
                }
                if (cs.NewProcessID == pid)
                {
                    result.SwitchIn++;
                    AddTransition(result, cs.NewThreadID, data.TimeStampQPC, CpuState.Running, "scheduled");
                }
            }
            else if (data is DispatcherReadyThreadTraceData ready && ready.AwakenedProcessID == pid)
            {
                NoteKernel(result, data.TimeStampQPC);
                result.Ready++;
                AddTransition(result, ready.AwakenedThreadID, data.TimeStampQPC, CpuState.Ready, "awakened");
                AddStack(log, data, ready.AwakenedThreadID, "ready-waker", result);
            }
            else if (data is SampledProfileTraceData sample && sample.ProcessID == pid)
            {
                NoteKernel(result, data.TimeStampQPC);
                result.Samples++;
                AddStack(log, data, sample.ThreadID, "sample", result);
            }
        }
        foreach (var timeline in result.Timelines.Values)
            timeline.Sort((a, b) => a.Qpc.CompareTo(b.Qpc));
        result.Stacks.Sort((a, b) => a.Qpc.CompareTo(b.Qpc));
        return result;
    }

    private static void NoteKernel(Collected data, long qpc)
    {
        data.KernelFirstQpc = Math.Min(data.KernelFirstQpc, qpc);
        data.KernelLastQpc = Math.Max(data.KernelLastQpc, qpc);
    }

    private static void ParseMarker(TraceEvent data, Collected result)
    {
        if (data.Version != MarkerVersion)
        {
            result.UnsupportedMarkers++;
            AddMarkerError(result, $"event {data.ID}: unsupported version {data.Version}");
            return;
        }
        var bytes = data.EventData();
        try
        {
            switch ((int)data.ID)
            {
                case 1 when bytes.Length == 32:
                    result.Clocks.Add(new ClockMarker(U64(bytes, 0), U64(bytes, 8), U64(bytes, 16),
                                                      U64(bytes, 24), data.TimeStampQPC));
                    break;
                case 2 when bytes.Length == 40:
                    result.Spans.Add(new SpanMarker(U64(bytes, 0), U64(bytes, 8), U64(bytes, 16),
                                                    I64(bytes, 24), U32(bytes, 32), U16(bytes, 36),
                                                    U16(bytes, 38)));
                    break;
                case 3 when bytes.Length == 96:
                    result.Frames.Add(new FrameMarker(U64(bytes, 0), U64(bytes, 8), U64(bytes, 16),
                        U64(bytes, 24), U64(bytes, 32), U64(bytes, 40), U64(bytes, 48), U64(bytes, 56),
                        U64(bytes, 64), U64(bytes, 72), U32(bytes, 80), U32(bytes, 84), U16(bytes, 88),
                        U16(bytes, 90), U32(bytes, 92)));
                    break;
                case 1 or 2 or 3:
                    result.MalformedMarkers++;
                    AddMarkerError(result, $"event {data.ID}: payload length {bytes.Length}");
                    break;
                default:
                    result.OtherMarkerEvents++;
                    break;
            }
        }
        catch (Exception ex)
        {
            result.MalformedMarkers++;
            AddMarkerError(result, $"event {data.ID}: {ex.Message}");
        }
    }

    private static void AddMarkerError(Collected result, string error)
    {
        if (result.MarkerErrors.Count < 20) result.MarkerErrors.Add(error);
    }

    private static ulong U64(byte[] b, int o) => BinaryPrimitives.ReadUInt64LittleEndian(b.AsSpan(o, 8));
    private static long I64(byte[] b, int o) => BinaryPrimitives.ReadInt64LittleEndian(b.AsSpan(o, 8));
    private static uint U32(byte[] b, int o) => BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(o, 4));
    private static ushort U16(byte[] b, int o) => BinaryPrimitives.ReadUInt16LittleEndian(b.AsSpan(o, 2));

    private static void AddTransition(Collected data, int thread, long qpc, CpuState state, string detail)
    {
        if (!data.Timelines.TryGetValue(thread, out var timeline))
            data.Timelines[thread] = timeline = [];
        timeline.Add(new Transition(qpc, state, detail));
    }

    private static CpuState ClassifyOldState(System.Diagnostics.ThreadState state) => state.ToString() switch
    {
        "Ready" or "Standby" or "DeferredReady" => CpuState.Ready,
        "Waiting" or "Transition" or "GateWait" or "WaitingForProcessInSwap" => CpuState.Waiting,
        _ => CpuState.Unknown,
    };

    private static void AddStack(TraceLog log, TraceEvent data, int targetThread, string kind, Collected result)
    {
        var stack = log.GetCallStackForEvent(data);
        if (stack is null) return;
        var frames = new List<string>();
        for (var frame = stack; frame is not null && frames.Count < 64; frame = frame.Caller)
        {
            var code = frame.CodeAddress;
            var module = code.ModuleFile;
            if (module is not null && code.Address >= module.ImageBase)
                frames.Add(FormatModuleAddress(module.FilePath, module.ImageBase, code.Address));
            else if (!string.IsNullOrWhiteSpace(code.ModuleName))
                frames.Add($"{code.ModuleName}@0x{code.Address:x}");
            else
                frames.Add($"0x{code.Address:x}");
        }
        result.Stacks.Add(new StackObservation(data.TimeStampQPC, targetThread, kind, frames.ToArray()));
    }

    private static string FormatModuleAddress(string path, ulong imageBase, ulong address) =>
        $"{path}+0x{address - imageBase:x} [va=0x{address:x}]";

    private static Dictionary<string, object?> BuildReport(string input, int pid, Collected data)
    {
        if (data.Clocks.Count == 0) throw new InvalidDataException("trace has no valid EDVR Clock marker");
        if (data.KernelFirstQpc == long.MaxValue || data.KernelLastQpc == long.MinValue)
            throw new InvalidDataException("trace has no kernel scheduler events");
        var frequencies = data.Clocks.Select(c => c.QpcFrequency).Where(f => f > 0).Distinct().ToArray();
        if (frequencies.Length != 1) throw new InvalidDataException("Clock markers have missing or inconsistent QPC frequencies");
        var frequency = frequencies[0];
        double QpcUs(long qpc) => qpc * 1_000_000.0 / frequency;
        var traceStartUs = QpcUs(data.KernelFirstQpc);
        var traceEndUs = QpcUs(data.KernelLastQpc);

        var clockResiduals = data.Clocks.Select(c =>
            Math.Abs((double)c.TimestampUs - c.QpcTicks * 1_000_000.0 / c.QpcFrequency)).ToArray();
        var clockHeaderDeltas = data.Clocks.Select(c => Math.Abs((double)c.HeaderQpc - c.QpcTicks)).ToArray();
        // EventWrite can be delayed after the clock sample. Only the payload's QPC/us
        // residual validates the mapping; header delay remains a diagnostic.
        var clocksValid = clockResiduals.All(x => x <= 2.0);

        var orderedFrames = data.Frames.OrderBy(f => f.TimestampUs).ToArray();
        var globalSequence = SequenceHealth(orderedFrames);
        var cohortSequence = SequenceHealth(orderedFrames.Where(frame =>
            frame.TimestampUs >= traceStartUs && frame.TimestampUs <= traceEndUs));
        var spans = data.Spans.Where(span => span.TimestampUs >= span.BeginUs).ToArray();
        var invalidSpans = data.Spans.Where(span => span.TimestampUs < span.BeginUs).ToArray();
        var retainedInvalidSpans = invalidSpans.Count(span =>
            span.TimestampUs >= traceStartUs && span.TimestampUs <= traceEndUs);
        var spanIndexes = spans.GroupBy(span => span.Thread)
            .ToDictionary(group => group.Key, group => new SpanCursor(group));
        var stackIndexes = data.Stacks.GroupBy(stack => stack.Thread)
            .ToDictionary(group => group.Key, group => group.OrderBy(stack => stack.Qpc).ToArray());
        var stackGroups = new Dictionary<string, (string Kind, string[] Frames, long Count)>();
        var frameReports = new List<Dictionary<string, object?>>();
        var invalidFrameReasons = new Dictionary<string, long>();
        var unavailableStatuses = new Dictionary<string, long>();
        var validFrames = new List<FrameMarker>();
        var retainedInvalidFrames = 0;
        foreach (var frame in orderedFrames)
        {
            if (frame.Status != 0)
            {
                Increment(unavailableStatuses, FrameStatusName(frame.Status));
                continue;
            }
            var invalid = FrameInvalidReason(frame);
            if (invalid is null) validFrames.Add(frame);
            else
            {
                Increment(invalidFrameReasons, invalid);
                if (frame.TimestampUs >= traceStartUs && frame.TimestampUs <= traceEndUs)
                    retainedInvalidFrames++;
            }
        }
        var discardedPrefix = 0;
        var discardedSuffix = 0;
        var discardedPartial = 0;
        var discardedUnknownState = 0;

        foreach (var frame in validFrames)
        {
            var start = (double)frame.PresentEndUs;
            var end = (double)frame.NextWaitEntryUs;
            if (end <= traceStartUs) { discardedPrefix++; continue; }
            if (start >= traceEndUs) { discardedSuffix++; continue; }
            if (start < traceStartUs || end > traceEndUs) { discardedPartial++; continue; }
            var thread = checked((int)frame.CallerThread);
            var stateUs = StateDurations(data.Timelines.GetValueOrDefault(thread), start, end, QpcUs);
            if (stateUs[CpuState.Unknown] > 0.001)
            {
                discardedUnknownState++;
                continue;
            }

            var frameStacks = stackIndexes.TryGetValue(thread, out var threadStacks)
                ? StacksInRange(threadStacks, start, end, QpcUs)
                : [];
            foreach (var observation in frameStacks)
            {
                var key = observation.Kind + "\n" + string.Join("\n", observation.Frames);
                if (stackGroups.TryGetValue(key, out var group))
                    stackGroups[key] = (group.Kind, group.Frames, group.Count + 1);
                else
                    stackGroups[key] = (observation.Kind, observation.Frames, 1);
            }

            var frameSpans = spanIndexes.TryGetValue(frame.CallerThread, out var spanIndex)
                ? spanIndex.Query(frame.PresentEndUs, frame.NextWaitEntryUs)
                : [];
            var operations = new Dictionary<string, object?>();
            foreach (var group in frameSpans.GroupBy(s => s.Operation).OrderBy(g => g.Key))
            {
                var clipped = group.Select(s => (Math.Max(start, s.BeginUs), Math.Min(end, s.TimestampUs))).ToArray();
                operations[OperationName(group.Key)] = new Dictionary<string, object?>
                {
                    ["count"] = group.Count(),
                    ["unionUs"] = Round(UnionDuration(clipped)),
                    ["results"] = group.GroupBy(s => s.Result).ToDictionary(g => g.Key.ToString(), g => g.Count()),
                };
            }
            var allClippedSpans = frameSpans.Select(s => (Math.Max(start, s.BeginUs), Math.Min(end, s.TimestampUs))).ToArray();
            var total = end - start;
            var exclusive = stateUs.Values.Sum();
            frameReports.Add(new Dictionary<string, object?>
            {
                ["sequence"] = frame.Sequence,
                ["generation"] = frame.Generation,
                ["featureEpoch"] = frame.FeatureEpoch,
                ["callerThread"] = frame.CallerThread,
                ["nextWaitThread"] = frame.NextWaitThread,
                ["status"] = frame.Status,
                ["flags"] = frame.Flags,
                ["sceneReady"] = frame.SceneReady,
                ["startUs"] = frame.PresentEndUs,
                ["endUs"] = frame.NextWaitEntryUs,
                ["durationUs"] = Round(total),
                ["runningUs"] = Round(stateUs[CpuState.Running]),
                ["readyUs"] = Round(stateUs[CpuState.Ready]),
                ["waitingUs"] = Round(stateUs[CpuState.Waiting]),
                ["unknownUs"] = Round(stateUs[CpuState.Unknown]),
                ["exclusiveSumUs"] = Round(exclusive),
                ["sampleStackCount"] = frameStacks.Count(s => s.Kind == "sample"),
                ["switchOutStackCount"] = frameStacks.Count(s => s.Kind == "switch-out"),
                ["readyWakerStackCount"] = frameStacks.Count(s => s.Kind == "ready-waker"),
                ["spanCount"] = frameSpans.Length,
                ["spanUnionUs"] = Round(UnionDuration(allClippedSpans)),
                ["operations"] = operations,
            });
        }

        var coverageComplete = CoverageComplete(data.EventsLost, data.MalformedMarkers,
            data.UnsupportedMarkers, cohortSequence.Gaps, cohortSequence.Duplicates,
            retainedInvalidSpans + retainedInvalidFrames, clocksValid,
            data.Frames.Count, frameReports.Count, data.SwitchOut, data.SwitchIn,
            data.HasCallStacks, frameReports.Select(f => Convert.ToDouble(f["unknownUs"])));
        var inputInfo = new FileInfo(input);
        return new Dictionary<string, object?>
        {
            ["schemaVersion"] = 1,
            ["input"] = input,
            ["pid"] = pid,
            ["providerGuid"] = EdvrProvider,
            ["qpcFrequency"] = frequency,
            ["schedulerTraceStartUs"] = Round(traceStartUs),
            ["schedulerTraceEndUs"] = Round(traceEndUs),
            ["schedulerTraceDurationMs"] = Round((traceEndUs - traceStartUs) / 1000.0),
            ["inputBytes"] = inputInfo.Length,
            ["inputMiBPerSecond"] = Round(inputInfo.Length / Math.Max(1.0, traceEndUs - traceStartUs) * 1_000_000.0 / 1048576.0),
            ["eventsLost"] = data.EventsLost,
            ["hasCallStacks"] = data.HasCallStacks,
            ["coverageComplete"] = coverageComplete,
            ["frameCount"] = data.Frames.Count,
            ["availableFrameCount"] = orderedFrames.Count(frame => frame.Status == 0),
            ["validFrameCount"] = validFrames.Count,
            ["analyzedFrameCount"] = frameReports.Count,
            ["discardedBeforeSchedulerTail"] = discardedPrefix,
            ["discardedAfterSchedulerTail"] = discardedSuffix,
            ["discardedPartialSchedulerBoundary"] = discardedPartial,
            ["discardedUnknownSchedulerState"] = discardedUnknownState,
            ["retainedInvalidFrameCount"] = retainedInvalidFrames,
            ["invalidFrameReasons"] = invalidFrameReasons,
            ["unavailableFrameStatuses"] = unavailableStatuses,
            ["clockCount"] = data.Clocks.Count,
            ["clocksValid"] = clocksValid,
            ["maxClockResidualUs"] = Round(clockResiduals.DefaultIfEmpty().Max()),
            ["maxClockHeaderDelayTicks"] = clockHeaderDeltas.DefaultIfEmpty().Max(),
            ["spanCount"] = data.Spans.Count,
            ["invalidSpanCount"] = invalidSpans.Length,
            ["retainedInvalidSpanCount"] = retainedInvalidSpans,
            ["malformedMarkerCount"] = data.MalformedMarkers,
            ["unsupportedMarkerCount"] = data.UnsupportedMarkers,
            ["otherMarkerEventCount"] = data.OtherMarkerEvents,
            ["markerErrors"] = data.MarkerErrors,
            ["observedSequenceGaps"] = globalSequence.Gaps,
            ["observedSequenceDuplicates"] = globalSequence.Duplicates,
            ["cohortSequenceGaps"] = cohortSequence.Gaps,
            ["cohortSequenceDuplicates"] = cohortSequence.Duplicates,
            ["switchOutCount"] = data.SwitchOut,
            ["switchInCount"] = data.SwitchIn,
            ["readyThreadCount"] = data.Ready,
            ["sampleCount"] = data.Samples,
            ["rawSwitchStates"] = data.RawSwitchStates,
            ["waitReasons"] = data.WaitReasons,
            ["topStacksInPostPresent"] = stackGroups.Values.OrderByDescending(g => g.Count).Take(50)
                .Select(g => new Dictionary<string, object?> { ["kind"] = g.Kind, ["count"] = g.Count, ["frames"] = g.Frames }).ToArray(),
            ["frames"] = frameReports,
        };
    }

    private static string? FrameInvalidReason(FrameMarker frame)
    {
        if ((frame.Flags & (PostValid | SinglePresent)) != (PostValid | SinglePresent)) return "required_flags";
        if (frame.Sequence == 0 || frame.Generation == 0 || frame.CallerThread == 0 || frame.NextWaitThread == 0)
            return "zero_identity";
        if (frame.CallerThread != frame.NextWaitThread) return "thread_mismatch";
        if (frame.TimestampUs != frame.NextWaitReturnUs) return "emission_timestamp";
        if (frame.WaitReturnUs == 0 || frame.SecondSubmitReturnUs == 0 || frame.PresentBeginUs == 0 ||
            frame.PresentEndUs == 0 || frame.NextWaitEntryUs == 0 || frame.NextWaitReturnUs == 0)
            return "zero_timestamp";
        if (!(frame.WaitReturnUs <= frame.SecondSubmitReturnUs &&
              frame.SecondSubmitReturnUs <= frame.PresentBeginUs &&
              frame.PresentBeginUs <= frame.PresentEndUs &&
              frame.PresentEndUs <= frame.NextWaitEntryUs &&
              frame.NextWaitEntryUs <= frame.NextWaitReturnUs))
            return "timestamp_order";
        return null;
    }

    private static string FrameStatusName(ushort status) => status switch
    {
        1 => "provider_missing", 2 => "bad_provider_version", 3 => "bad_provider_size",
        4 => "bad_provider_generation", 5 => "not_yet_observable", 6 => "lost_present_history",
        7 => "partial_present", 8 => "wrong_present_thread", 9 => "failed_present",
        10 => "test_present", 11 => "malformed_present", _ => $"unknown_{status}",
    };

    private static StackObservation[] StacksInRange(StackObservation[] stacks, double start, double end,
                                                     Func<long, double> qpcUs)
    {
        var low = LowerBound(stacks.Length, index => qpcUs(stacks[index].Qpc) >= start);
        var high = LowerBound(stacks.Length, index => qpcUs(stacks[index].Qpc) >= end);
        return stacks[low..high];
    }

    private static int LowerBound(int count, Func<int, bool> predicate)
    {
        var low = 0;
        var high = count;
        while (low < high)
        {
            var middle = low + (high - low) / 2;
            if (predicate(middle)) high = middle;
            else low = middle + 1;
        }
        return low;
    }

    private static bool CoverageComplete(int eventsLost, int malformedMarkers, int unsupportedMarkers,
                                         int sequenceGaps, int sequenceDuplicates, int invalidSpans,
                                         bool clocksValid, int frameCount,
                                         int analyzedFrameCount, long switchOut, long switchIn,
                                         bool hasCallStacks, IEnumerable<double> unknownDurations) =>
        eventsLost == 0 && malformedMarkers == 0 && unsupportedMarkers == 0 && sequenceGaps == 0 &&
        sequenceDuplicates == 0 && invalidSpans == 0 && clocksValid &&
        frameCount > 0 && analyzedFrameCount > 0 && switchOut > 0 && switchIn > 0 && hasCallStacks &&
        unknownDurations.All(value => value <= 0.001);

    private sealed record SequenceResult(int Gaps, int Duplicates);
    private static SequenceResult SequenceHealth(IEnumerable<FrameMarker> frames)
    {
        var gaps = 0;
        var duplicates = 0;
        foreach (var generation in frames.GroupBy(f => (f.Generation, f.FeatureEpoch)))
        {
            ulong? previous = null;
            foreach (var frame in generation.OrderBy(f => f.Sequence))
            {
                if (previous == frame.Sequence) duplicates++;
                else if (previous is not null && frame.Sequence > previous.Value + 1)
                    gaps += checked((int)Math.Min(int.MaxValue, frame.Sequence - previous.Value - 1));
                previous = frame.Sequence;
            }
        }
        return new SequenceResult(gaps, duplicates);
    }

    private static Dictionary<CpuState, double> StateDurations(List<Transition>? timeline, double start,
                                                                 double end, Func<long, double> qpcUs)
    {
        var values = Enum.GetValues<CpuState>().ToDictionary(s => s, _ => 0.0);
        if (end <= start) return values;
        var state = CpuState.Unknown;
        var cursor = start;
        if (timeline is null)
        {
            values[state] = end - start;
            return values;
        }
        var index = LowerBound(timeline.Count, i => qpcUs(timeline[i].Qpc) > start);
        if (index > 0) state = timeline[index - 1].State;
        while (index < timeline.Count)
        {
            var next = qpcUs(timeline[index].Qpc);
            if (next >= end) break;
            if (next > cursor) values[state] += next - cursor;
            cursor = Math.Max(cursor, next);
            state = timeline[index++].State;
        }
        if (cursor < end) values[state] += end - cursor;
        return values;
    }

    private static double UnionDuration(IEnumerable<(double Start, double End)> ranges)
    {
        var ordered = ranges.Where(r => r.End > r.Start).OrderBy(r => r.Start).ThenBy(r => r.End).ToArray();
        if (ordered.Length == 0) return 0;
        var total = 0.0;
        var start = ordered[0].Start;
        var end = ordered[0].End;
        for (var i = 1; i < ordered.Length; i++)
        {
            if (ordered[i].Start <= end) end = Math.Max(end, ordered[i].End);
            else { total += end - start; start = ordered[i].Start; end = ordered[i].End; }
        }
        return total + end - start;
    }

    private static string OperationName(ushort operation) => operation switch
    {
        1 => "WaitGetPoses",
        2 => "Submit",
        3 => "PostPresentHandoff",
        4 => "GetLastPoses",
        5 => "GetLastPoseForTrackedDeviceIndex",
        6 => "GetFrameTiming",
        7 => "GetFrameTimeRemaining",
        8 => "CanRenderScene",
        9 => "GetTimeSinceLastVsync",
        10 => "GetDeviceToAbsoluteTrackingPose",
        0x100 => "SyntheticCycle",
        0x101 => "SyntheticBusy",
        0x102 => "SyntheticWait",
        _ => $"Unknown{operation}",
    };

    private static void Increment(Dictionary<string, long> counts, string key) =>
        counts[key] = counts.GetValueOrDefault(key) + 1;
    private static double Round(double value) => Math.Round(value, 3, MidpointRounding.AwayFromZero);

    private static void SelfTest()
    {
        var timeline = new List<Transition>
        {
            new(0, CpuState.Running, "start"), new(10, CpuState.Waiting, "sleep"),
            new(20, CpuState.Ready, "wake"), new(25, CpuState.Running, "scheduled"),
            new(40, CpuState.Ready, "preempted"), new(45, CpuState.Running, "scheduled"),
        };
        var state = StateDurations(timeline, 5, 50, q => q);
        AssertNear(state[CpuState.Running], 25, "running");
        AssertNear(state[CpuState.Ready], 10, "ready");
        AssertNear(state[CpuState.Waiting], 10, "waiting");
        AssertNear(state.Values.Sum(), 45, "exclusive state sum");

        var boundary = StateDurations(timeline, -5, 5, q => q);
        AssertNear(boundary[CpuState.Unknown], 5, "unknown prefix");
        AssertNear(boundary[CpuState.Running], 5, "boundary transition");
        AssertNear(UnionDuration([(12, 20), (15, 25), (30, 35), (35, 35)]), 18, "nested span union");

        var spanCursor = new SpanCursor([
            new SpanMarker(20, 1, 10, 0, 7, 1, 0),
            new SpanMarker(25, 2, 15, 0, 7, 2, 0),
            new SpanMarker(35, 3, 30, 0, 7, 1, 0),
        ]);
        if (spanCursor.Query(12, 22).Length != 2 || spanCursor.Query(22, 32).Length != 2)
            throw new Exception("span window index test failed");

        var validFrame = new FrameMarker(60, 1, 1, 0, 10, 20, 50, 60, 30, 40, 7, 7, 0,
                                         PostValid | SinglePresent, 1);
        if (FrameInvalidReason(validFrame) is not null) throw new Exception("valid frame rejected");
        if (FrameInvalidReason(validFrame with { NextWaitThread = 8 }) != "thread_mismatch")
            throw new Exception("frame thread admission test failed");
        if (FrameInvalidReason(validFrame with { PresentEndUs = 55 }) != "timestamp_order")
            throw new Exception("frame timestamp admission test failed");
        var moduleAddress = FormatModuleAddress(@"C:\Games\Elite\d3d11.dll", 0x180000000, 0x180001234);
        if (moduleAddress != @"C:\Games\Elite\d3d11.dll+0x1234 [va=0x180001234]")
            throw new Exception("module path/RVA test failed");

        var frames = new[]
        {
            new FrameMarker(0, 10, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0),
            new FrameMarker(0, 12, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0),
            new FrameMarker(0, 12, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0),
            new FrameMarker(0, 1, 2, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0),
        };
        var health = SequenceHealth(frames);
        if (health.Gaps != 1 || health.Duplicates != 1) throw new Exception("sequence coverage test failed");
        if (CoverageComplete(1, 0, 0, 0, 0, 0, true, 1, 1, 1, 1, true, [0]))
            throw new Exception("lost-event coverage test failed");
        if (CoverageComplete(0, 0, 0, 0, 0, 0, true, 0, 0, 0, 0, true, []))
            throw new Exception("zero marker/scheduler coverage test failed");

        var clock = new byte[32];
        BinaryPrimitives.WriteUInt64LittleEndian(clock.AsSpan(0), 11);
        BinaryPrimitives.WriteUInt64LittleEndian(clock.AsSpan(8), 22);
        BinaryPrimitives.WriteUInt64LittleEndian(clock.AsSpan(16), 33);
        BinaryPrimitives.WriteUInt64LittleEndian(clock.AsSpan(24), 44);
        if (U64(clock, 0) != 11 || U64(clock, 8) != 22 || U64(clock, 16) != 33 || U64(clock, 24) != 44)
            throw new Exception("marker schema offsets failed");
    }

    private static void AssertNear(double actual, double expected, string name)
    {
        if (Math.Abs(actual - expected) > 0.0001) throw new Exception($"{name}: expected {expected}, got {actual}");
    }
}
