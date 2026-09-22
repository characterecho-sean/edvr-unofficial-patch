using System.Text.Json;

// report.json keeps every key earlier sessions and tools/cpu_profile.py already
// read, with the same meaning, and gains the aggregate views. The per-frame
// detail is written to frames.jsonl beside it so the report stays small on a
// four-minute trace; report.json names that file in framesPath.

/// Spans sorted by begin with a running maximum end, so "every span overlapping
/// [start, end)" is a binary search plus a short backward walk even when a
/// WaitGetPoses span is milliseconds long.
internal sealed class SpanIndex
{
    private readonly SpanMarker[] _spans;
    private readonly ulong[] _maxEnd;

    public SpanIndex(IEnumerable<SpanMarker> spans)
    {
        _spans = spans.OrderBy(span => span.BeginUs).ToArray();
        _maxEnd = new ulong[_spans.Length];
        ulong running = 0;
        for (var i = 0; i < _spans.Length; i++)
        {
            running = Math.Max(running, _spans[i].TimestampUs);
            _maxEnd[i] = running;
        }
    }

    public List<SpanMarker> Overlapping(ulong start, ulong end)
    {
        var found = new List<SpanMarker>();
        var high = Numeric.LowerBound(_spans.Length, index => _spans[index].BeginUs >= end);
        for (var i = high - 1; i >= 0 && _maxEnd[i] > start; i--)
            if (_spans[i].TimestampUs > start && _spans[i].BeginUs < end) found.Add(_spans[i]);
        found.Reverse();
        return found;
    }
}

internal sealed record WindowGroup(int Number, string Source, LogWindow? Log, List<FrameCycle> Frames);

internal static class Report
{
    public const int FixedWindowUs = 5_000_000;

    public static Dictionary<string, object?> Build(string input, int pid, Collected data, string framesPath,
                                                    RuntimeLogResult? runtimeLog, Action<string>? progress = null)
    {
        if (data.Clocks.Count == 0) throw new InvalidDataException("trace has no valid EDVR Clock marker");
        if (data.KernelFirstQpc == long.MaxValue || data.KernelLastQpc == long.MinValue)
            throw new InvalidDataException("trace has no kernel scheduler events");
        var frequencies = data.Clocks.Select(c => c.QpcFrequency).Where(f => f > 0).Distinct().ToArray();
        if (frequencies.Length != 1)
            throw new InvalidDataException("Clock markers have missing or inconsistent QPC frequencies");
        var frequency = frequencies[0];
        double QpcUs(long qpc) => qpc * 1_000_000.0 / frequency;
        var globalTraceStartUs = QpcUs(data.KernelFirstQpc);
        // A circular system collector can retain a different oldest buffer on each observed CPU.
        // All scheduler streams are present only after the latest of those retained starts.
        var commonTraceStartUs = QpcUs(CommonSchedulerStart(data.CpuSwitchRanges));
        var traceStartUs = commonTraceStartUs;
        var traceEndUs = QpcUs(data.KernelLastQpc);

        var clockResiduals = data.Clocks.Select(c =>
            Math.Abs((double)c.TimestampUs - c.QpcTicks * 1_000_000.0 / c.QpcFrequency)).ToArray();
        var clockHeaderDeltas = data.Clocks.Select(c => Math.Abs((double)c.HeaderQpc - c.QpcTicks)).ToArray();
        // EventWrite can be delayed after the clock sample. Only the payload's QPC/us
        // residual validates the mapping; header delay remains a diagnostic.
        var clocksValid = clockResiduals.All(x => x <= 2.0);
        var clockOffsets = data.Clocks.Select(c => c.SystemTime100ns / 10.0 - c.TimestampUs).ToArray();

        var orderedFrames = data.Frames.OrderBy(f => f.TimestampUs).ToArray();
        var globalSequence = Markers.SequenceHealth(orderedFrames);
        var cohortSequence = Markers.SequenceHealth(orderedFrames.Where(frame =>
            frame.TimestampUs >= traceStartUs && frame.TimestampUs <= traceEndUs));
        var spans = data.Spans.Where(span => span.TimestampUs >= span.BeginUs).ToArray();
        var invalidSpans = data.Spans.Where(span => span.TimestampUs < span.BeginUs).ToArray();
        var retainedInvalidSpans = invalidSpans.Count(span =>
            span.TimestampUs >= traceStartUs && span.TimestampUs <= traceEndUs);
        var spanIndexes = spans.GroupBy(span => span.Thread)
            .ToDictionary(group => group.Key, group => new SpanIndex(group));

        var invalidFrameReasons = new Dictionary<string, long>();
        var unavailableStatuses = new Dictionary<string, long>();
        var validFrames = new List<FrameMarker>();
        var retainedInvalidFrames = 0;
        foreach (var frame in orderedFrames)
        {
            if (frame.Status != 0)
            {
                Numeric.Increment(unavailableStatuses, Markers.FrameStatusName(frame.Status));
                continue;
            }
            var invalid = Markers.FrameInvalidReason(frame);
            if (invalid is null) validFrames.Add(frame);
            else
            {
                Numeric.Increment(invalidFrameReasons, invalid);
                if (frame.TimestampUs >= traceStartUs && frame.TimestampUs <= traceEndUs)
                    retainedInvalidFrames++;
            }
        }

        var analyzer = new CycleAnalyzer(data, pid, QpcUs, traceStartUs, traceEndUs);
        var cycles = validFrames.Select(analyzer.Analyze).ToList();
        progress?.Invoke($"cycles: valid={cycles.Count} derived={analyzer.DerivedFrames} " +
                         $"covered={cycles.Count(c => c.Covered)}");

        var groups = Group(cycles, runtimeLog);
        var stackGroups = new Dictionary<(string Kind, int StackId), long>();
        var frameReports = new List<Dictionary<string, object?>>();
        var discardedPrefix = 0;
        var discardedSuffix = 0;
        var discardedPartial = 0;
        var discardedBoundaryUnknownState = 0;
        foreach (var cycle in cycles)
        {
            if (cycle.LegacyCovered) frameReports.Add(LegacyFrame(cycle, data, QpcUs, spanIndexes, stackGroups));
            else switch (cycle.LegacyDiscardReason)
            {
                case "prefix": discardedPrefix++; break;
                case "suffix": discardedSuffix++; break;
                case "partial": discardedPartial++; break;
                case "boundary_unknown": discardedBoundaryUnknownState++; break;
            }
        }

        var covered = cycles.Where(cycle => cycle.Covered).ToList();
        WriteFrames(framesPath, cycles, data);
        var maxRegionResidual = covered.SelectMany(cycle => cycle.Regions)
            .Select(region => Math.Abs(region.ResidualUs)).DefaultIfEmpty(0).Max();
        var maxUnknownUs = covered.SelectMany(cycle => cycle.Regions)
            .Select(region => region.States.Unknown).DefaultIfEmpty(0).Max();

        var coverageComplete = CoverageComplete(data.EventsLost, data.MalformedMarkers,
            data.UnsupportedMarkers, cohortSequence.Gaps, cohortSequence.Duplicates,
            retainedInvalidSpans + retainedInvalidFrames, clocksValid,
            data.Frames.Count, frameReports.Count, data.SwitchOut, data.SwitchIn,
            data.HasCallStacks, frameReports.Select(f => Convert.ToDouble(f["unexpectedUnknownUs"])));
        var inputInfo = new FileInfo(input);
        var report = new Dictionary<string, object?>
        {
            ["schemaVersion"] = 1,
            ["input"] = input,
            ["pid"] = pid,
            ["providerGuid"] = Collector.EdvrProvider,
            ["qpcFrequency"] = frequency,
            ["schedulerTraceStartUs"] = Numeric.Round(traceStartUs),
            ["schedulerTraceGlobalStartUs"] = Numeric.Round(globalTraceStartUs),
            ["schedulerTraceCommonStartUs"] = Numeric.Round(commonTraceStartUs),
            ["schedulerTraceEndUs"] = Numeric.Round(traceEndUs),
            ["schedulerTraceDurationMs"] = Numeric.Round((traceEndUs - traceStartUs) / 1000.0),
            ["schedulerTraceGlobalDurationMs"] = Numeric.Round((traceEndUs - globalTraceStartUs) / 1000.0),
            ["schedulerTraceCommonDurationMs"] = Numeric.Round((traceEndUs - commonTraceStartUs) / 1000.0),
            ["inputBytes"] = inputInfo.Length,
            ["inputMiBPerSecond"] = Numeric.Round(inputInfo.Length /
                Math.Max(1.0, traceEndUs - globalTraceStartUs) * 1_000_000.0 / 1048576.0),
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
            ["discardedUnknownSchedulerState"] = discardedBoundaryUnknownState,
            ["discardedBoundaryUnknownState"] = discardedBoundaryUnknownState,
            ["unexpectedUnknownFrameCount"] = frameReports.Count(frame =>
                Convert.ToDouble(frame["unexpectedUnknownUs"]) > 0.001),
            ["retainedInvalidFrameCount"] = retainedInvalidFrames,
            ["invalidFrameReasons"] = invalidFrameReasons,
            ["unavailableFrameStatuses"] = unavailableStatuses,
            ["clockCount"] = data.Clocks.Count,
            ["clocksValid"] = clocksValid,
            ["maxClockResidualUs"] = Numeric.Round(clockResiduals.DefaultIfEmpty().Max()),
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
            ["switchOutUnresolvedProcess"] = data.SwitchOutUnresolvedProcess,
            ["switchInUnresolvedProcess"] = data.SwitchInUnresolvedProcess,
            ["switchOutForeignProcessKnownThread"] = data.SwitchOutForeignProcessKnownThread,
            ["switchOutBlockingStackCount"] = data.SwitchOutBlockingStacks,
            ["switchOutBlockingStackOwnerMismatchCount"] = data.SwitchOutBlockingStackOwnerMismatches,
            ["readyThreadCount"] = data.Ready,
            ["sampleCount"] = data.Samples,
            ["rawSwitchStates"] = data.RawSwitchStates,
            ["waitReasons"] = data.WaitReasons,
            ["perCpuSwitchRanges"] = data.CpuSwitchRanges.OrderBy(pair => pair.Key)
                .Select(pair => new Dictionary<string, object?>
                {
                    ["processor"] = pair.Key,
                    ["firstUs"] = Numeric.Round(QpcUs(pair.Value.FirstQpc)),
                    ["lastUs"] = Numeric.Round(QpcUs(pair.Value.LastQpc)),
                    ["count"] = pair.Value.Count,
                }).ToArray(),
            ["topStacksInPostPresent"] = TopStacks(stackGroups, data.Stacks, 50),
            ["topStacksInPostPresentByKind"] = TopStacksByKind(stackGroups, data.Stacks),
            ["frames"] = frameReports,

            // --- added by the cycle analyzer ---
            ["framesPath"] = framesPath,
            ["frameDetailCount"] = cycles.Count,
            ["cycleDerivedFrameCount"] = analyzer.DerivedFrames,
            ["cycleCoveredFrameCount"] = covered.Count,
            ["cycleDerivationFailures"] = analyzer.DerivationFailures,
            ["maxRegionResidualUs"] = Numeric.Round(maxRegionResidual),
            ["maxRegionUnknownUs"] = Numeric.Round(maxUnknownUs),
            ["sampleIntervalUs"] = Numeric.Round(data.SampleIntervalUs),
            ["sampleIntervalSource"] = data.SampleIntervalSource,
            ["sampleIntervalFromLogUs"] = Numeric.Round(data.SampleIntervalFromLogUs),
            ["sampleIntervalFromEventsUs"] = Numeric.Round(data.SampleIntervalFromEventsUs),
            ["callerThreads"] = data.CallerThreads.OrderBy(x => x).ToArray(),
            ["distinctStackCount"] = data.Stacks.Count,
            ["unsortedTimelineCount"] = data.UnsortedTimelines,
            ["stackOwnership"] = new Dictionary<string, object?>
            {
                // ETW semantics, measured instead of assumed: a ReadyThread stack
                // belongs to the thread that emitted the event (the waker), not to
                // the awakened thread.
                ["readyStacks"] = data.ReadyStacks,
                ["readyStackOwnedByEmitter"] = data.ReadyStackEmitterOwned,
                ["readyStackOwnedByAwakened"] = data.ReadyStackAwakenedOwned,
                ["readyStackOwnedByOther"] = data.ReadyStackUnownedOwner,
                ["sampleStacks"] = data.SampleStacks,
                ["sampleStackOwnerMismatches"] = data.SampleStackOwnerMismatches,
                ["switchOutBlockingStacks"] = data.SwitchOutBlockingStacks,
                ["switchOutBlockingStackOwnerMismatches"] = data.SwitchOutBlockingStackOwnerMismatches,
            },
            ["clockMapping"] = new Dictionary<string, object?>
            {
                ["utcMinusTimestampUsMin"] = Numeric.Round(clockOffsets.DefaultIfEmpty().Min()),
                ["utcMinusTimestampUsMax"] = Numeric.Round(clockOffsets.DefaultIfEmpty().Max()),
                ["utcMinusTimestampUsSpread"] = Numeric.Round(
                    clockOffsets.DefaultIfEmpty().Max() - clockOffsets.DefaultIfEmpty().Min()),
                ["firstFrameWaitReturnUtc"] = cycles.Count > 0 ? Markers.UtcText(cycles[0].WaitReturnUtc) : "",
                ["lastFrameWaitReturnUtc"] = cycles.Count > 0 ? Markers.UtcText(cycles[^1].WaitReturnUtc) : "",
                ["schedulerStartUtc"] = Markers.UtcText(Markers.Utc(
                    data.Clocks.OrderBy(c => c.TimestampUs).ToArray(), (ulong)Math.Max(0, traceStartUs))),
                ["schedulerEndUtc"] = Markers.UtcText(Markers.Utc(
                    data.Clocks.OrderBy(c => c.TimestampUs).ToArray(), (ulong)Math.Max(0, traceEndUs))),
            },
            ["spanOffsets"] = SpanOffsets(cycles),
            ["cycleCoveredWithoutDerivation"] = analyzer.CoveredWithoutDerivation,
            ["threads"] = Threads(covered, data),
            ["callerStacksByRegion"] = CallerStacksByRegion(covered, data),
            ["rvaClasses"] = RvaClasses(covered, data, analyzer),
            ["attributionSummary"] = Attribution(covered, data),
            ["gate"] = Gate(covered),
            ["windows"] = groups.Select(group => Window(group, data)).ToArray(),
            ["runtimeLog"] = runtimeLog is null ? null : new Dictionary<string, object?>
            {
                ["path"] = runtimeLog.Path,
                ["windowLines"] = runtimeLog.WindowLines,
                ["phaseLines"] = runtimeLog.PhaseLines,
                ["unparsedLines"] = runtimeLog.UnparsedLines,
                ["foreignPidLines"] = runtimeLog.ForeignPidLines,
                ["windows"] = runtimeLog.Windows.Count,
            },
        };
        return report;
    }

    private static List<WindowGroup> Group(List<FrameCycle> cycles, RuntimeLogResult? runtimeLog)
    {
        var groups = new List<WindowGroup>();
        if (runtimeLog is not null && runtimeLog.Windows.Count > 0)
        {
            var byNumber = new Dictionary<int, WindowGroup>();
            foreach (var window in runtimeLog.Windows)
            {
                var group = new WindowGroup(window.Window, "runtime_log", window, []);
                byNumber[window.Window] = group;
                groups.Add(group);
            }
            foreach (var cycle in cycles)
            {
                var window = runtimeLog.Windows.FirstOrDefault(w =>
                    cycle.Marker.Sequence >= w.First && cycle.Marker.Sequence <= w.Last &&
                    (w.Generation == 0 || w.Generation == cycle.Marker.Generation) &&
                    (w.FeatureEpoch == 0 || w.FeatureEpoch == cycle.Marker.FeatureEpoch));
                if (window is null) continue;
                cycle.WindowNumber = window.Window;
                byNumber[window.Window].Frames.Add(cycle);
            }
            return groups;
        }
        if (cycles.Count == 0) return groups;
        var anchor = cycles.Min(cycle => cycle.WaitReturnUs);
        var fixedGroups = new Dictionary<int, WindowGroup>();
        foreach (var cycle in cycles)
        {
            var number = (int)((cycle.WaitReturnUs - anchor) / FixedWindowUs) + 1;
            if (!fixedGroups.TryGetValue(number, out var group))
            {
                fixedGroups[number] = group = new WindowGroup(number, "fixed_5s", null, []);
                groups.Add(group);
            }
            cycle.WindowNumber = number;
            group.Frames.Add(cycle);
        }
        groups.Sort((a, b) => a.Number.CompareTo(b.Number));
        return groups;
    }

    private static Dictionary<string, object?> Window(WindowGroup group, Collected data)
    {
        var frames = group.Frames;
        var derived = frames.Where(frame => frame.Derived).ToList();
        var covered = frames.Where(frame => frame.Covered).ToList();
        var phases = new List<Dictionary<string, object?>>();
        foreach (var name in RuntimeLog.PrimaryPhases)
        {
            var values = derived.Select(frame => frame.PhaseMs(name)).ToList();
            var trace = Dist.Of(values);
            var log = group.Log?.Phases.GetValueOrDefault(name);
            phases.Add(new Dictionary<string, object?>
            {
                ["name"] = name,
                ["logMeanMs"] = log is null ? null : (object?)log.Mean,
                ["traceMeanMs"] = Numeric.Round4(trace.Mean),
                ["deltaMs"] = log is null ? null : (object?)Numeric.Round4(trace.Mean - log.Mean),
                ["logP50Ms"] = log?.P50,
                ["traceP50Ms"] = Numeric.Round4(trace.P50),
                ["logP95Ms"] = log?.P95,
                ["traceP95Ms"] = Numeric.Round4(trace.P95),
                ["traceFrames"] = trace.Count,
            });
        }
        var result = new Dictionary<string, object?>
        {
            ["window"] = group.Number,
            ["source"] = group.Source,
            ["firstSequence"] = group.Log?.First ?? frames.Select(f => f.Marker.Sequence).DefaultIfEmpty().Min(),
            ["lastSequence"] = group.Log?.Last ?? frames.Select(f => f.Marker.Sequence).DefaultIfEmpty().Max(),
            ["logAdmitted"] = group.Log?.Admitted,
            ["logValid"] = group.Log?.Valid,
            ["logElapsedMs"] = group.Log?.ElapsedMs,
            ["logEmittedUtc"] = group.Log is null ? null : Markers.UtcText(group.Log.EmittedUtc),
            ["logValidCycleSumMs"] = group.Log?.ValidCycleSumMs,
            ["traceFrames"] = frames.Count,
            ["traceDerivedFrames"] = derived.Count,
            ["traceCoveredFrames"] = covered.Count,
            ["traceFirstSequence"] = frames.Select(f => f.Marker.Sequence).DefaultIfEmpty().Min(),
            ["traceLastSequence"] = frames.Select(f => f.Marker.Sequence).DefaultIfEmpty().Max(),
            ["firstWaitReturnUtc"] = frames.Count > 0 ? Markers.UtcText(frames[0].WaitReturnUtc) : "",
            ["lastWaitReturnUtc"] = frames.Count > 0 ? Markers.UtcText(frames[^1].WaitReturnUtc) : "",
            ["phases"] = phases,
            ["spanOffsets"] = SpanOffsets(frames),
            ["regions"] = Regions(covered),
            ["threads"] = Threads(covered, data),
            ["callerStacksByRegion"] = CallerStacksByRegion(covered, data),
            ["attribution"] = Attribution(covered, data),
            ["gate"] = Gate(covered),
        };
        return result;
    }

    private static Dictionary<string, object?> SpanOffsets(List<FrameCycle> frames) => new()
    {
        // Every derived boundary is a span edge, and a span starts before the
        // runtime takes its own tick and ends after it. These are the measured
        // sizes of that gap, which is what any trace-minus-log delta is made of.
        ["secondSubmitSpanEndMinusMarkerUs"] = Numeric.Json(Dist.Of(
            frames.Where(f => f.SubmitOffsetKnown).Select(f => f.SubmitSpanEndOffsetUs).ToList())),
        ["nextWaitEntryMinusWaitSpanBeginUs"] = Numeric.Json(Dist.Of(
            frames.Where(f => f.WaitOffsetKnown).Select(f => f.WaitSpanBeginOffsetUs).ToList())),
        ["waitSpanEndMinusNextWaitReturnUs"] = Numeric.Json(Dist.Of(
            frames.Where(f => f.WaitOffsetKnown).Select(f => f.WaitSpanEndOffsetUs).ToList())),
    };

    private static object Regions(List<FrameCycle> frames)
    {
        var rows = new List<Dictionary<string, object?>>();
        foreach (var name in CycleAnalyzer.RegionNames)
        {
            var regions = frames.Select(frame => frame.Region(name)).Where(region => region is not null)
                .Select(region => region!).ToList();
            if (regions.Count == 0) continue;
            rows.Add(new Dictionary<string, object?>
            {
                ["name"] = name,
                ["frames"] = regions.Count,
                ["meanLengthUs"] = Numeric.Round(regions.Average(r => r.LengthUs)),
                ["meanRunningUs"] = Numeric.Round(regions.Average(r => r.States.Running)),
                ["meanReadyUs"] = Numeric.Round(regions.Average(r => r.States.Ready)),
                ["meanWaitingUs"] = Numeric.Round(regions.Average(r => r.States.Waiting)),
                ["meanUnknownUs"] = Numeric.Round(regions.Average(r => r.States.Unknown)),
                ["maxResidualUs"] = Numeric.Round(regions.Max(r => Math.Abs(r.ResidualUs))),
            });
        }
        return rows;
    }

    private static object Threads(List<FrameCycle> frames, Collected data)
    {
        var running = new Dictionary<int, double>();
        var samples = new Dictionary<int, long>();
        var pipeline = new Dictionary<int, long>();
        var classes = new Dictionary<int, long[]>();
        foreach (var frame in frames)
            foreach (var thread in frame.Threads)
            {
                running[thread.ThreadId] = running.GetValueOrDefault(thread.ThreadId) + thread.RunningUs;
                samples[thread.ThreadId] = samples.GetValueOrDefault(thread.ThreadId) + thread.Samples;
                pipeline[thread.ThreadId] = pipeline.GetValueOrDefault(thread.ThreadId) + thread.PipelineSamples;
                if (thread.ClassSamples is null) continue;
                if (!classes.TryGetValue(thread.ThreadId, out var totals))
                    classes[thread.ThreadId] = totals = new long[RvaTable.Classes.Length];
                for (var i = 0; i < totals.Length; i++) totals[i] += thread.ClassSamples[i];
            }
        var interval = data.SampleIntervalUs;
        return running.Keys.OrderByDescending(thread => running[thread]).Select(thread =>
        {
            var row = new Dictionary<string, object?>
            {
                ["threadId"] = thread,
                ["isCallerThread"] = data.CallerThreads.Contains(thread),
                ["runningUs"] = Numeric.Round(running[thread]),
                ["runningUsPerFrame"] = Numeric.Round(frames.Count > 0 ? running[thread] / frames.Count : 0),
                ["switchIns"] = data.SwitchInByThread.GetValueOrDefault(thread),
                ["switchOuts"] = data.SwitchOutByThread.GetValueOrDefault(thread),
                ["samples"] = samples.GetValueOrDefault(thread),
                ["sampleRunningUs"] = Numeric.Round(samples.GetValueOrDefault(thread) * interval),
                ["pipelineSamples"] = pipeline.GetValueOrDefault(thread),
                ["pipelineRunningUs"] = Numeric.Round(pipeline.GetValueOrDefault(thread) * interval),
            };
            if (classes.TryGetValue(thread, out var totals))
            {
                var byClass = new Dictionary<string, object?>();
                for (var i = 0; i < totals.Length; i++)
                    if (totals[i] > 0) byClass[RvaTable.Classes[i].Name] = totals[i];
                row["samplesByRvaClass"] = byClass;
            }
            return row;
        }).ToArray();
    }

    private static Dictionary<string, object?> RvaClasses(List<FrameCycle> frames, Collected data,
                                                          CycleAnalyzer analyzer)
    {
        var definitions = RvaTable.Classes.Select(item => new Dictionary<string, object?>
        {
            ["name"] = item.Name,
            ["entryRva"] = $"0x{item.Entry:x}",
            ["boundRva"] = $"0x{item.Bound:x}",
            ["kind"] = item.Kind.ToString().ToLowerInvariant(),
            ["pipeline"] = item.Pipeline,
            ["boundRule"] = RvaTable.BoundRule(item),
        }).ToArray();
        var sampleCounts = new Dictionary<string, object?>();
        var firstSeen = new Dictionary<string, object?>();
        var lastSeen = new Dictionary<string, object?>();
        for (var i = 0; i < RvaTable.Classes.Length; i++)
        {
            // null, not zero: a class no thread ever sampled inside has no first
            // or last frame, which is the answer for a one-shot candidate that
            // never ran in the window with stacks.
            sampleCounts[RvaTable.Classes[i].Name] = analyzer.ClassSampleTotals[i];
            firstSeen[RvaTable.Classes[i].Name] =
                analyzer.ClassFirstSequence[i] == 0 ? null : analyzer.ClassFirstSequence[i];
            lastSeen[RvaTable.Classes[i].Name] =
                analyzer.ClassLastSequence[i] == 0 ? null : analyzer.ClassLastSequence[i];
        }
        var callerLabels = new Dictionary<string, long>();
        foreach (var frame in frames)
            for (var i = 0; i < frame.CallerClassSamples.Length; i++)
                if (frame.CallerClassSamples[i] > 0)
                    callerLabels[RvaTable.Classes[i].Name] =
                        callerLabels.GetValueOrDefault(RvaTable.Classes[i].Name) + frame.CallerClassSamples[i];
        return new Dictionary<string, object?>
        {
            ["definitions"] = definitions,
            ["gameDirectory"] = data.Stacks.GameDirectory,
            // Every module a stack frame landed in, with how many interned frames
            // landed there. Nothing is filtered out: a module missing from this
            // list is a module no sampled or switched stack ever reached.
            ["modules"] = data.Stacks.Modules
                .OrderByDescending(module => module.Frames).ThenBy(module => module.Name)
                .Select(module => new Dictionary<string, object?>
                {
                    ["name"] = module.Display,
                    ["path"] = module.Path,
                    ["imageBase"] = $"0x{module.ImageBase:x}",
                    ["class"] = module.Class.ToString(),
                    ["frames"] = module.Frames,
                }).ToArray(),
            ["frameResolution"] = new Dictionary<string, object?>
            {
                // Per distinct stack frame, not per observation.
                ["resolvedByTraceEvent"] = data.Stacks.ResolvedFrames,
                ["recoveredFromLoadedModules"] = data.Stacks.RecoveredFrames,
                ["unresolved"] = data.Stacks.UnresolvedFrames,
            },
            ["sampleCountsByClass"] = sampleCounts,
            ["callerSampleCountsByClass"] = callerLabels,
            ["firstSampledFrameSequence"] = firstSeen,
            ["lastSampledFrameSequence"] = lastSeen,
            ["totalCoveredFrames"] = frames.Count,
        };
    }

    private static Dictionary<string, object?> Attribution(List<FrameCycle> frames, Collected data)
    {
        var segments = frames.SelectMany(frame => frame.Waits).ToList();
        var byClass = new Dictionary<string, (long Count, double Us)>();
        foreach (var segment in segments)
        {
            var entry = byClass.GetValueOrDefault(segment.WakerClass);
            byClass[segment.WakerClass] = (entry.Count + 1, entry.Us + segment.InsideUs);
        }
        var totalUs = segments.Sum(segment => segment.InsideUs);
        var blocking = new Dictionary<string, (long Count, double Us)>();
        foreach (var segment in segments)
        {
            var key = Site(segment.BlockingTops);
            var entry = blocking.GetValueOrDefault(key);
            blocking[key] = (entry.Count + 1, entry.Us + segment.InsideUs);
        }
        var wakerTops = new Dictionary<string, (long Count, double Us)>();
        foreach (var segment in segments)
        {
            var key = segment.WakerReadyStackId < 0 ? "none" : Site(segment.WakerReadyTops);
            var entry = wakerTops.GetValueOrDefault(key);
            wakerTops[key] = (entry.Count + 1, entry.Us + segment.InsideUs);
        }
        var wakerThreads = new Dictionary<int, (long Count, double Us)>();
        foreach (var segment in segments)
        {
            var entry = wakerThreads.GetValueOrDefault(segment.WakerThread);
            wakerThreads[segment.WakerThread] = (entry.Count + 1, entry.Us + segment.InsideUs);
        }
        return new Dictionary<string, object?>
        {
            ["frames"] = frames.Count,
            ["waitSegments"] = segments.Count,
            ["waitUs"] = Numeric.Round(totalUs),
            ["waitUsPerFrame"] = Numeric.Round(frames.Count > 0 ? totalUs / frames.Count : 0),
            ["callerSwitchOutStacks"] = frames.Sum(frame => (long)frame.CallerSwitchOutStacks),
            ["callerReadyStacks"] = frames.Sum(frame => (long)frame.CallerReadyStacks),
            ["byWakerClass"] = byClass.OrderByDescending(pair => pair.Value.Us)
                .Select(pair => new Dictionary<string, object?>
                {
                    ["class"] = pair.Key,
                    ["segments"] = pair.Value.Count,
                    ["us"] = Numeric.Round(pair.Value.Us),
                    ["usPerFrame"] = Numeric.Round(frames.Count > 0 ? pair.Value.Us / frames.Count : 0),
                    ["shareOfWaitUs"] = Numeric.Round4(totalUs > 0 ? pair.Value.Us / totalUs : 0),
                }).ToArray(),
            ["byRegion"] = RegionSplit(segments, frames.Count),
            ["regionSplitResidualUs"] = Numeric.Round(segments
                .Select(segment => Math.Abs(segment.RegionUs.Sum() - segment.InsideUs))
                .DefaultIfEmpty(0).Max()),
            ["byBlockingSiteAndWakerClass"] = SiteCrossTab(segments, frames.Count, 12),
            ["byBlockingSite"] = Top(blocking, totalUs, 20),
            ["byWakerReadyTop"] = Top(wakerTops, totalUs, 20),
            ["byWakerThread"] = wakerThreads.OrderByDescending(pair => pair.Value.Us).Take(20)
                .Select(pair => new Dictionary<string, object?>
                {
                    ["threadId"] = pair.Key,
                    ["isCallerThread"] = data.CallerThreads.Contains(pair.Key),
                    ["segments"] = pair.Value.Count,
                    ["us"] = Numeric.Round(pair.Value.Us),
                }).ToArray(),
            ["wakerSampleAgeUs"] = Numeric.Json(Dist.Of(
                segments.Where(segment => segment.WakerSampleFound)
                    .Select(segment => segment.WakerSampleAgeUs).ToList())),
            ["pipelineWaitShare"] = Numeric.Round4(totalUs > 0
                ? byClass.GetValueOrDefault("pipeline").Us / totalUs : 0),
        };
    }

    /// What the caller thread was executing, by coarse region of the cycle: the
    /// innermost frame's module, the modules and classes anywhere in the stack,
    /// and the stacks themselves. One sample is one sampling interval, so a
    /// count is also a time.
    public static object CallerStacksByRegion(List<FrameCycle> frames, Collected data, int topGroups = 30)
    {
        var rows = new List<Dictionary<string, object?>>();
        var interval = data.SampleIntervalUs;
        for (byte group = 0; group < CycleAnalyzer.SampleRegionNames.Length; group++)
        {
            var byModule = new Dictionary<string, long>();
            var bySignature = new Dictionary<string, long>();
            var edvrInnermost = new Dictionary<string, long>();
            var edvrChains = new Dictionary<(string Chain, string Site), long>();
            long total = 0, edvr = 0, system = 0, nvidia = 0, pipeline = 0, census = 0, scheduler = 0;
            long topUnresolved = 0, anyUnresolved = 0;
            foreach (var frame in frames)
                foreach (var (sampleGroup, stackId) in frame.CallerSamplesByRegion)
                {
                    if (sampleGroup != group || stackId < 0) continue;
                    var stack = data.Stacks[stackId];
                    total++;
                    Numeric.Increment(byModule, data.Stacks.DisplayName(stack.TopModuleId));
                    Numeric.Increment(bySignature, data.Stacks.Signature(stack));
                    if (stack.AnyEdvrD3d11) edvr++;
                    if (stack.AnySystemD3d11) system++;
                    if (stack.AnyNvidiaUserMode) nvidia++;
                    if (stack.Pipeline) pipeline++;
                    if (stack.Census) census++;
                    if (stack.Scheduler) scheduler++;
                    if (stack.TopUnresolved) topUnresolved++;
                    if (stack.AnyUnresolved) anyUnresolved++;
                    if (!stack.AnyEdvrD3d11) continue;
                    var chain = data.Stacks.EdvrFrames(stack, 6, out var site);
                    if (chain.Count == 0) continue;
                    Numeric.Increment(edvrInnermost, chain[0]);
                    Numeric.Increment(edvrChains, (string.Join(" < ", chain), site));
                }
            rows.Add(new Dictionary<string, object?>
            {
                ["region"] = CycleAnalyzer.SampleRegionNames[group],
                ["samples"] = total,
                ["msPerFrame"] = Numeric.Round(frames.Count > 0 ? total * interval / 1000.0 / frames.Count : 0),
                ["byTopModule"] = byModule.OrderByDescending(pair => pair.Value)
                    .Select(pair => new Dictionary<string, object?>
                    {
                        ["module"] = pair.Key,
                        ["samples"] = pair.Value,
                        ["msPerFrame"] = Numeric.Round(frames.Count > 0
                            ? pair.Value * interval / 1000.0 / frames.Count : 0),
                        ["share"] = Numeric.Round4(total > 0 ? (double)pair.Value / total : 0),
                    }).ToArray(),
                // These overlap by construction: one stack can pass through the
                // game, EDVR's d3d11 and the display driver on its way down.
                ["anyFrameIn"] = new Dictionary<string, object?>
                {
                    ["edvrD3d11"] = edvr,
                    ["systemD3d11"] = system,
                    ["nvidiaUserMode"] = nvidia,
                    ["pipelineClass"] = pipeline,
                    ["censusBracket"] = census,
                    ["schedulerRange"] = scheduler,
                    ["topFrameUnresolved"] = topUnresolved,
                    ["anyFrameUnresolved"] = anyUnresolved,
                },
                ["topStacks"] = bySignature.OrderByDescending(pair => pair.Value).Take(topGroups)
                    .Select(pair => new Dictionary<string, object?>
                    {
                        ["count"] = pair.Value,
                        ["stack"] = pair.Key,
                    }).ToArray(),
                // RVAs inside EDVR's own d3d11.dll, for symbolizing against the
                // built DLL: the innermost frame of each sample, and the chain
                // through our hook with the game call site that entered it.
                ["edvrInnermostRvas"] = edvrInnermost.OrderByDescending(pair => pair.Value).Take(40)
                    .Select(pair => new Dictionary<string, object?>
                    {
                        ["rva"] = pair.Key,
                        ["count"] = pair.Value,
                    }).ToArray(),
                ["edvrChains"] = edvrChains.OrderByDescending(pair => pair.Value).Take(25)
                    .Select(pair => new Dictionary<string, object?>
                    {
                        ["chain"] = pair.Key.Chain.Split(" < "),
                        ["gameCallSite"] = pair.Key.Site,
                        ["count"] = pair.Value,
                    }).ToArray(),
            });
        }
        return rows;
    }

    /// Wait time by region of the cycle crossed with the waker class, so R1's
    /// own waits read separately from the pacer block inside R4.
    private static object RegionSplit(List<WaitSegment> segments, int frames)
    {
        var rows = new List<Dictionary<string, object?>>();
        for (var i = 0; i < CycleAnalyzer.PartitionRegionNames.Length; i++)
        {
            var byClass = new Dictionary<string, (long Count, double Us)>();
            var total = 0.0;
            foreach (var segment in segments)
            {
                if (i >= segment.RegionUs.Length) continue;
                var us = segment.RegionUs[i];
                if (us <= 0) continue;
                var entry = byClass.GetValueOrDefault(segment.WakerClass);
                byClass[segment.WakerClass] = (entry.Count + 1, entry.Us + us);
                total += us;
            }
            rows.Add(new Dictionary<string, object?>
            {
                ["region"] = CycleAnalyzer.PartitionRegionNames[i],
                ["segments"] = byClass.Values.Sum(value => value.Count),
                ["waitUs"] = Numeric.Round(total),
                ["waitUsPerFrame"] = Numeric.Round(frames > 0 ? total / frames : 0),
                ["byClass"] = ByClass(byClass, total, frames),
            });
        }
        return rows;
    }

    /// The largest blocking sites by wait time, each split by waker class: a
    /// scheduler-site wait whose waker was stale or idle is a different finding
    /// from one whose waker had just been in a job body.
    private static object SiteCrossTab(List<WaitSegment> segments, int frames, int take)
    {
        var sites = new Dictionary<string, double>();
        foreach (var segment in segments)
        {
            var key = Site(segment.BlockingTops);
            sites[key] = sites.GetValueOrDefault(key) + segment.InsideUs;
        }
        var rows = new List<Dictionary<string, object?>>();
        foreach (var site in sites.OrderByDescending(pair => pair.Value).Take(take))
        {
            var byClass = new Dictionary<string, (long Count, double Us)>();
            var count = 0L;
            foreach (var segment in segments.Where(segment => Site(segment.BlockingTops) == site.Key))
            {
                var entry = byClass.GetValueOrDefault(segment.WakerClass);
                byClass[segment.WakerClass] = (entry.Count + 1, entry.Us + segment.InsideUs);
                count++;
            }
            rows.Add(new Dictionary<string, object?>
            {
                ["site"] = site.Key,
                ["segments"] = count,
                ["waitUs"] = Numeric.Round(site.Value),
                ["waitUsPerFrame"] = Numeric.Round(frames > 0 ? site.Value / frames : 0),
                ["byClass"] = ByClass(byClass, site.Value, frames),
            });
        }
        return rows;
    }

    private static Dictionary<string, object?> RegionUs(WaitSegment segment)
    {
        var map = new Dictionary<string, object?>();
        for (var i = 0; i < segment.RegionUs.Length && i < CycleAnalyzer.PartitionRegionNames.Length; i++)
            if (segment.RegionUs[i] > 0)
                map[CycleAnalyzer.PartitionRegionNames[i]] = Numeric.Round(segment.RegionUs[i]);
        return map;
    }

    private static object ByClass(Dictionary<string, (long Count, double Us)> byClass, double totalUs,
                                  int frames) =>
        byClass.OrderByDescending(pair => pair.Value.Us).Select(pair => new Dictionary<string, object?>
        {
            ["class"] = pair.Key,
            ["segments"] = pair.Value.Count,
            ["us"] = Numeric.Round(pair.Value.Us),
            ["usPerFrame"] = Numeric.Round(frames > 0 ? pair.Value.Us / frames : 0),
            ["share"] = Numeric.Round4(totalUs > 0 ? pair.Value.Us / totalUs : 0),
        }).ToArray();

    private static object Top(Dictionary<string, (long Count, double Us)> table, double totalUs, int take) =>
        table.OrderByDescending(pair => pair.Value.Count).Take(take)
            .Select(pair => new Dictionary<string, object?>
            {
                ["site"] = pair.Key,
                ["segments"] = pair.Value.Count,
                ["us"] = Numeric.Round(pair.Value.Us),
                ["shareOfWaitUs"] = Numeric.Round4(totalUs > 0 ? pair.Value.Us / totalUs : 0),
            }).ToArray();

    public static string Site(GameTop tops)
    {
        if (tops.Count == 0) return "no_game_frame";
        if (tops.Count == 1) return $"0x{tops.First:x}";
        if (tops.Count == 2) return $"0x{tops.First:x} < 0x{tops.Second:x}";
        return $"0x{tops.First:x} < 0x{tops.Second:x} < 0x{tops.Third:x}";
    }

    private static Dictionary<string, object?> Gate(List<FrameCycle> frames)
    {
        var callerRunning = frames.Sum(frame => frame.CallerPipelineRunningUs);
        var callerWait = frames.Sum(frame => frame.PipelineWaitUs);
        var threadRunning = frames.Sum(frame => frame.ThreadPipelineRunningUs);
        return new Dictionary<string, object?>
        {
            ["frames"] = frames.Count,
            ["callerPipelineRunningUs"] = Numeric.Round(callerRunning),
            ["callerPipelineRunningUsPerFrame"] = Numeric.Round(frames.Count > 0 ? callerRunning / frames.Count : 0),
            ["callerPipelineWaitUs"] = Numeric.Round(callerWait),
            ["callerPipelineWaitUsPerFrame"] = Numeric.Round(frames.Count > 0 ? callerWait / frames.Count : 0),
            ["threadPipelineRunningUs"] = Numeric.Round(threadRunning),
            ["threadPipelineRunningUsPerFrame"] = Numeric.Round(frames.Count > 0 ? threadRunning / frames.Count : 0),
            ["criticalPathShare"] = Numeric.Round4(threadRunning > 0
                ? (callerRunning + callerWait) / threadRunning : 0),
            ["criticalPathShareMeanOfFrames"] = Numeric.Round4(frames.Count > 0
                ? frames.Average(frame => frame.CriticalPathShare) : 0),
        };
    }

    private static Dictionary<string, object?> LegacyFrame(FrameCycle cycle, Collected data,
        Func<long, double> qpcUs, Dictionary<uint, SpanIndex> spanIndexes,
        Dictionary<(string Kind, int StackId), long> stackGroups)
    {
        var frame = cycle.Marker;
        var start = (double)frame.PresentEndUs;
        var end = (double)frame.NextWaitEntryUs;
        var region = cycle.LegacyRegion!;
        var thread = checked((int)frame.CallerThread);
        var sampleStacks = Observations(data.SamplesByThread, thread, start, end, qpcUs);
        var switchStacks = Observations(data.SwitchOutStacksByThread, thread, start, end, qpcUs);
        var readyStacks = new List<int>();
        if (data.ReadiesByThread.TryGetValue(thread, out var readies))
        {
            var low = Numeric.LowerBound(readies.Count, index => qpcUs(readies[index].Qpc) >= start);
            for (var i = low; i < readies.Count && qpcUs(readies[i].Qpc) < end; i++)
                if (readies[i].StackId >= 0) readyStacks.Add(readies[i].StackId);
        }
        foreach (var id in sampleStacks) Bump(stackGroups, ("sample", id));
        foreach (var id in switchStacks) Bump(stackGroups, ("switch-out", id));
        foreach (var id in readyStacks) Bump(stackGroups, ("ready-waker", id));

        var frameSpans = spanIndexes.TryGetValue(frame.CallerThread, out var index)
            ? index.Overlapping(frame.PresentEndUs, frame.NextWaitEntryUs) : [];
        var operations = new Dictionary<string, object?>();
        foreach (var group in frameSpans.GroupBy(span => span.Operation).OrderBy(group => group.Key))
        {
            var clipped = group.Select(span =>
                (Math.Max(start, span.BeginUs), Math.Min(end, span.TimestampUs))).ToArray();
            operations[Markers.OperationName(group.Key)] = new Dictionary<string, object?>
            {
                ["count"] = group.Count(),
                ["unionUs"] = Numeric.Round(Numeric.UnionDuration(clipped)),
                ["results"] = group.GroupBy(span => span.Result)
                    .ToDictionary(item => item.Key.ToString(), item => item.Count()),
            };
        }
        var allClipped = frameSpans.Select(span =>
            (Math.Max(start, span.BeginUs), Math.Min(end, span.TimestampUs))).ToArray();
        return new Dictionary<string, object?>
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
            ["durationUs"] = Numeric.Round(end - start),
            ["runningUs"] = Numeric.Round(region.States.Running),
            ["readyUs"] = Numeric.Round(region.States.Ready),
            ["waitingUs"] = Numeric.Round(region.States.Waiting),
            ["unknownUs"] = Numeric.Round(region.States.UnexpectedUnknown),
            ["unexpectedUnknownUs"] = Numeric.Round(region.States.UnexpectedUnknown),
            ["exclusiveSumUs"] = Numeric.Round(region.States.Total),
            ["sampleStackCount"] = sampleStacks.Count,
            ["switchOutStackCount"] = switchStacks.Count,
            ["readyWakerStackCount"] = readyStacks.Count,
            ["spanCount"] = frameSpans.Count,
            ["spanUnionUs"] = Numeric.Round(Numeric.UnionDuration(allClipped)),
            ["operations"] = operations,
        };
    }

    private static List<int> Observations(Dictionary<int, List<StackObs>> map, int thread, double start,
                                          double end, Func<long, double> qpcUs)
    {
        var found = new List<int>();
        if (!map.TryGetValue(thread, out var list)) return found;
        var low = Numeric.LowerBound(list.Count, index => qpcUs(list[index].Qpc) >= start);
        for (var i = low; i < list.Count && qpcUs(list[i].Qpc) < end; i++) found.Add(list[i].StackId);
        return found;
    }

    private static void Bump(Dictionary<(string Kind, int StackId), long> groups, (string, int) key) =>
        groups[key] = groups.GetValueOrDefault(key) + 1;

    private static object TopStacks(Dictionary<(string Kind, int StackId), long> groups, StackStore stacks,
                                    int take) =>
        groups.OrderByDescending(pair => pair.Value).Take(take)
            .Select(pair => new Dictionary<string, object?>
            {
                ["kind"] = pair.Key.Kind,
                ["count"] = pair.Value,
                ["frames"] = stacks.Format(stacks[pair.Key.StackId]),
            }).ToArray();

    private static Dictionary<string, object?> TopStacksByKind(
        Dictionary<(string Kind, int StackId), long> groups, StackStore stacks) =>
        groups.GroupBy(pair => pair.Key.Kind).OrderBy(group => group.Key)
            .ToDictionary(group => group.Key, group => (object?)group.OrderByDescending(pair => pair.Value)
                .Take(50).Select(pair => new Dictionary<string, object?>
                {
                    ["count"] = pair.Value,
                    ["frames"] = stacks.Format(stacks[pair.Key.StackId]),
                }).ToArray());

    private static long CommonSchedulerStart(Dictionary<int, CpuSwitchRange> ranges)
    {
        if (ranges.Count == 0) throw new InvalidDataException("trace has no per-CPU context-switch ranges");
        return ranges.Values.Max(range => range.FirstQpc);
    }

    public static bool CoverageComplete(int eventsLost, int malformedMarkers, int unsupportedMarkers,
                                        int sequenceGaps, int sequenceDuplicates, int invalidSpans,
                                        bool clocksValid, int frameCount,
                                        int analyzedFrameCount, long switchOut, long switchIn,
                                        bool hasCallStacks, IEnumerable<double> unknownDurations) =>
        eventsLost == 0 && malformedMarkers == 0 && unsupportedMarkers == 0 && sequenceGaps == 0 &&
        sequenceDuplicates == 0 && invalidSpans == 0 && clocksValid &&
        frameCount > 0 && analyzedFrameCount > 0 && switchOut > 0 && switchIn > 0 && hasCallStacks &&
        unknownDurations.All(value => value <= 0.001);

    private static void WriteFrames(string path, List<FrameCycle> cycles, Collected data)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        using var writer = new StreamWriter(path, false);
        foreach (var cycle in cycles) writer.WriteLine(JsonSerializer.Serialize(FrameDetail(cycle, data)));
    }

    public static Dictionary<string, object?> FrameDetail(FrameCycle cycle, Collected data)
    {
        var detail = new Dictionary<string, object?>
        {
            ["sequence"] = cycle.Marker.Sequence,
            ["generation"] = cycle.Marker.Generation,
            ["featureEpoch"] = cycle.Marker.FeatureEpoch,
            ["callerThread"] = cycle.Marker.CallerThread,
            ["sceneReady"] = cycle.Marker.SceneReady,
            ["window"] = cycle.WindowNumber < 0 ? null : cycle.WindowNumber,
            ["derived"] = cycle.Derived,
            ["derivationReason"] = cycle.DerivationReason,
            ["covered"] = cycle.Covered,
            ["timestamps"] = new Dictionary<string, object?>
            {
                ["waitReturnUs"] = cycle.WaitReturnUs,
                ["firstSubmitEntryUs"] = cycle.FirstSubmitEntryUs,
                ["firstSubmitReturnUs"] = cycle.FirstSubmitReturnUs,
                ["secondSubmitEntryUs"] = cycle.SecondSubmitEntryUs,
                ["secondSubmitReturnUs"] = cycle.SecondSubmitReturnUs,
                ["presentBeginUs"] = cycle.PresentBeginUs,
                ["presentEndUs"] = cycle.PresentEndUs,
                ["nextWaitEntryUs"] = cycle.NextWaitEntryUs,
                ["nextWaitReturnUs"] = cycle.NextWaitReturnUs,
                ["waitReturnUtc"] = Markers.UtcText(cycle.WaitReturnUtc),
            },
            ["phasesMs"] = RuntimeLog.PrimaryPhases.ToDictionary(name => name,
                name => (object?)Numeric.Round4(cycle.PhaseMs(name))),
            ["spanOffsetsUs"] = new Dictionary<string, object?>
            {
                ["secondSubmitSpanEndMinusMarker"] = cycle.SubmitOffsetKnown
                    ? Numeric.Round(cycle.SubmitSpanEndOffsetUs) : null,
                ["nextWaitEntryMinusWaitSpanBegin"] = cycle.WaitOffsetKnown
                    ? Numeric.Round(cycle.WaitSpanBeginOffsetUs) : null,
                ["waitSpanEndMinusNextWaitReturn"] = cycle.WaitOffsetKnown
                    ? Numeric.Round(cycle.WaitSpanEndOffsetUs) : null,
            },
        };
        if (!cycle.Covered) return detail;
        detail["regions"] = cycle.Regions.Select(region => new Dictionary<string, object?>
        {
            ["name"] = region.Name,
            ["startUs"] = Numeric.Round(region.StartUs),
            ["endUs"] = Numeric.Round(region.EndUs),
            ["lengthUs"] = Numeric.Round(region.LengthUs),
            ["runningUs"] = Numeric.Round(region.States.Running),
            ["readyUs"] = Numeric.Round(region.States.Ready),
            ["waitingUs"] = Numeric.Round(region.States.Waiting),
            ["unknownUs"] = Numeric.Round(region.States.Unknown),
            ["residualUs"] = Numeric.Round(region.ResidualUs),
        }).ToArray();
        detail["threads"] = cycle.Threads.Where(thread => thread.RunningUs > 0 || thread.Samples > 0)
            .OrderByDescending(thread => thread.RunningUs).Select(thread =>
            {
                var row = new Dictionary<string, object?>
                {
                    ["threadId"] = thread.ThreadId,
                    ["runningUs"] = Numeric.Round(thread.RunningUs),
                    ["samples"] = thread.Samples,
                    ["pipelineSamples"] = thread.PipelineSamples,
                };
                if (thread.ClassSamples is not null)
                {
                    var byClass = new Dictionary<string, object?>();
                    for (var i = 0; i < thread.ClassSamples.Length; i++)
                        if (thread.ClassSamples[i] > 0) byClass[RvaTable.Classes[i].Name] = thread.ClassSamples[i];
                    row["samplesByRvaClass"] = byClass;
                }
                return row;
            }).ToArray();
        detail["waits"] = cycle.Waits.Select(segment => new Dictionary<string, object?>
        {
            ["startUs"] = Numeric.Round(segment.StartUs),
            ["endUs"] = Numeric.Round(segment.EndUs),
            ["lengthUs"] = Numeric.Round(segment.LengthUs),
            ["insideCycleUs"] = Numeric.Round(segment.InsideUs),
            ["endedReady"] = segment.EndedReady,
            ["blockingSite"] = Site(segment.BlockingTops),
            ["wakerThread"] = segment.WakerThread,
            ["wakerProcess"] = segment.WakerProcess,
            ["wakerReadyTop"] = segment.WakerReadyStackId < 0 ? null : Site(segment.WakerReadyTops),
            ["wakerClass"] = segment.WakerClass,
            ["wakerSampleAgeUs"] = segment.WakerSampleFound ? Numeric.Round(segment.WakerSampleAgeUs) : null,
            ["regionUs"] = RegionUs(segment),
        }).ToArray();
        var callerByClass = new Dictionary<string, object?>();
        for (var i = 0; i < cycle.CallerClassSamples.Length; i++)
            if (cycle.CallerClassSamples[i] > 0)
                callerByClass[RvaTable.Classes[i].Name] = cycle.CallerClassSamples[i];
        detail["caller"] = new Dictionary<string, object?>
        {
            ["samples"] = cycle.CallerSamples,
            ["pipelineSamples"] = cycle.CallerPipelineSamples,
            ["switchOutStacks"] = cycle.CallerSwitchOutStacks,
            ["readyStacks"] = cycle.CallerReadyStacks,
            ["samplesByRvaClass"] = callerByClass,
        };
        detail["gate"] = new Dictionary<string, object?>
        {
            ["callerPipelineRunningUs"] = Numeric.Round(cycle.CallerPipelineRunningUs),
            ["callerPipelineWaitUs"] = Numeric.Round(cycle.PipelineWaitUs),
            ["threadPipelineRunningUs"] = Numeric.Round(cycle.ThreadPipelineRunningUs),
            ["criticalPathShare"] = Numeric.Round4(cycle.CriticalPathShare),
        };
        return detail;
    }
}
