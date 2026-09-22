using System.Buffers.Binary;

// Every computation in this analyzer is exercised here against synthetic data
// whose answer is known by hand. A parser that drifts from the format it reads
// must fail in build.bat, not in the ten minutes after a flight finally
// reproduced the effect being chased.

internal static class SelfTests
{
    private static int _checks;

    public static void Run()
    {
        SchedulerStateMapping();
        StateDurationWalk();
        RunningWalk();
        Percentiles();
        Unions();
        SpanIndexWindows();
        MarkerAdmission();
        MarkerSchema();
        ClockMapping();
        RvaClassTable();
        StackLabels();
        RuntimeLogParsing();
        CycleDerivation();
        CycleRegions();
        WaitAttribution();
        ThreadBusyAndGate();
        WindowGrouping();
        ReportShape();
        Console.WriteLine($"EdvrCpuProfile self-test: {_checks} checks");
    }

    private static void Check(bool condition, string name)
    {
        _checks++;
        if (!condition) throw new Exception($"self-test failed: {name}");
    }

    private static void Near(double actual, double expected, string name, double tolerance = 0.0001)
    {
        _checks++;
        if (Math.Abs(actual - expected) > tolerance)
            throw new Exception($"self-test failed: {name}: expected {expected}, got {actual}");
    }

    private static void SchedulerStateMapping()
    {
        Check(Collector.ClassifyOldState(1) == CpuState.Ready, "raw state 1 is ready");
        Check(Collector.ClassifyOldState(2) == CpuState.Running, "raw state 2 is running");
        Check(Collector.ClassifyOldState(3) == CpuState.Ready, "raw state 3 is ready");
        Check(Collector.ClassifyOldState(5) == CpuState.Waiting, "raw state 5 is waiting");
        Check(Collector.ClassifyOldState(6) == CpuState.Waiting, "raw state 6 is waiting");
        Check(Collector.ClassifyOldState(7) == CpuState.Ready, "raw state 7 is deferred ready");
        Check(Collector.ClassifyOldState(99) == CpuState.UnexpectedUnknown, "unknown raw state");
        Check(Collector.StackOwnerMatches(9392, 17948, 9392, 17948), "blocking stack owner accepted");
        Check(!Collector.StackOwnerMatches(5116, 14276, 9392, 17948), "foreign process stack rejected");
        Check(!Collector.StackOwnerMatches(9392, 14276, 9392, 17948), "foreign thread stack rejected");
    }

    private static ThreadTimeline Timeline(params (long Qpc, CpuState State)[] transitions)
    {
        var timeline = new ThreadTimeline();
        foreach (var item in transitions) timeline.Add(item.Qpc, item.State);
        return timeline;
    }

    private static void StateDurationWalk()
    {
        var timeline = Timeline((0, CpuState.Running), (10, CpuState.Waiting), (20, CpuState.Ready),
                                (25, CpuState.Running), (40, CpuState.Ready), (45, CpuState.Running));
        var state = Numeric.Durations(timeline, 5, 50, q => q);
        Near(state.Running, 25, "running");
        Near(state.Ready, 10, "ready");
        Near(state.Waiting, 10, "waiting");
        Near(state.Total, 45, "exclusive state sum");

        var boundary = Numeric.Durations(timeline, -5, 5, q => q);
        Near(boundary.BoundaryUnknown, 5, "unknown prefix");
        Near(boundary.Running, 5, "boundary transition");

        var unexpected = Numeric.Durations(Timeline((0, CpuState.Running), (10, CpuState.UnexpectedUnknown),
                                                    (20, CpuState.Running)), 5, 15, q => q);
        Near(unexpected.UnexpectedUnknown, 5, "unexpected unknown interval");
        Check(!Report.CoverageComplete(0, 0, 0, 0, 0, 0, true, 1, 1, 1, 1, true, [unexpected.UnexpectedUnknown]),
              "unexpected unknown fails coverage");
        Check(!Report.CoverageComplete(1, 0, 0, 0, 0, 0, true, 1, 1, 1, 1, true, [0]), "lost events fail coverage");
        Check(!Report.CoverageComplete(0, 0, 0, 0, 0, 0, true, 0, 0, 0, 0, true, []), "empty trace fails coverage");
        Check(Report.CoverageComplete(0, 0, 0, 0, 0, 0, true, 1, 1, 1, 1, true, [0]), "clean trace passes coverage");

        // A switch-out and the switch-in that follows it can share one QPC tick.
        // Transitions are appended in trace order, so the later one wins.
        var equal = Timeline((10, CpuState.Ready), (10, CpuState.Running));
        Near(Numeric.Durations(equal, 10, 20, q => q).Running, 10, "equal-QPC event order");
        Check(equal.Sorted(), "equal-QPC timeline is sorted");
        Check(!Timeline((10, CpuState.Running), (5, CpuState.Running)).Sorted(), "out-of-order timeline detected");
    }

    private static void RunningWalk()
    {
        var timeline = Timeline((0, CpuState.Running), (10, CpuState.Waiting), (20, CpuState.Running));
        Near(Numeric.RunningUs(timeline, 0, 30, q => q), 20, "running walk matches");
        Near(Numeric.RunningUs(timeline, 0, 30, q => q),
             Numeric.Durations(timeline, 0, 30, q => q).Running, "running walk equals state walk");
        Near(Numeric.RunningUs(null, 0, 30, q => q), 0, "running walk without a timeline");
    }

    private static void Percentiles()
    {
        double[] sorted = [1, 2, 3, 4];
        Near(Numeric.Percentile(sorted, 50), 2, "p50 integer rule");
        Near(Numeric.Percentile(sorted, 95), 4, "p95 integer rule");
        var dist = Dist.Of([3, 1, 2]);
        Near(dist.Mean, 2, "dist mean");
        Near(dist.P50, 2, "dist p50");
        Near(dist.Min, 1, "dist min");
        Near(dist.Max, 3, "dist max");
        Check(Dist.Of([]).Count == 0, "empty dist");
    }

    private static void Unions() =>
        Near(Numeric.UnionDuration([(12, 20), (15, 25), (30, 35), (35, 35)]), 18, "nested span union");

    private static void SpanIndexWindows()
    {
        var index = new SpanIndex([
            new SpanMarker(20, 1, 10, 0, 7, 1, 0),
            new SpanMarker(25, 2, 15, 0, 7, 2, 0),
            new SpanMarker(35, 3, 30, 0, 7, 1, 0),
        ]);
        Check(index.Overlapping(12, 22).Count == 2, "span window head");
        Check(index.Overlapping(22, 32).Count == 2, "span window tail");
        Check(index.Overlapping(36, 40).Count == 0, "span window past the end");
        // A WaitGetPoses span can be milliseconds long and start well before the
        // window; the running maximum end is what finds it.
        var longSpan = new SpanIndex([
            new SpanMarker(1000, 1, 0, 0, 7, 1, 0),
            new SpanMarker(60, 2, 50, 0, 7, 2, 0),
        ]);
        Check(longSpan.Overlapping(900, 950).Count == 1, "long span found from behind");
    }

    private static void MarkerAdmission()
    {
        var valid = new FrameMarker(60, 1, 1, 0, 10, 20, 50, 60, 30, 40, 7, 7, 0,
                                    Markers.PostValid | Markers.SinglePresent, 1);
        Check(Markers.FrameInvalidReason(valid) is null, "valid frame admitted");
        Check(Markers.FrameInvalidReason(valid with { NextWaitThread = 8 }) == "thread_mismatch", "thread mismatch");
        Check(Markers.FrameInvalidReason(valid with { PresentEndUs = 55 }) == "timestamp_order", "timestamp order");
        Check(Markers.FrameInvalidReason(valid with { Flags = 0 }) == "required_flags", "required flags");
        Check(Markers.FrameStatusName(6) == "lost_present_history", "status name");
        Check(Markers.OperationName(2) == "Submit", "operation name");

        var health = Markers.SequenceHealth([
            new FrameMarker(0, 10, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0),
            new FrameMarker(0, 12, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0),
            new FrameMarker(0, 12, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0),
            new FrameMarker(0, 1, 2, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0),
        ]);
        Check(health.Gaps == 1 && health.Duplicates == 1, "sequence health");
        Check(StackStore.FormatModuleAddress(@"C:\Games\Elite\d3d11.dll", 0x180000000, 0x180001234) ==
              @"C:\Games\Elite\d3d11.dll+0x1234 [va=0x180001234]", "module path and RVA");
    }

    private static void MarkerSchema()
    {
        var clock = new byte[32];
        BinaryPrimitives.WriteUInt64LittleEndian(clock.AsSpan(0), 11);
        BinaryPrimitives.WriteUInt64LittleEndian(clock.AsSpan(8), 22);
        BinaryPrimitives.WriteUInt64LittleEndian(clock.AsSpan(16), 33);
        BinaryPrimitives.WriteUInt64LittleEndian(clock.AsSpan(24), 44);
        Check(Markers.U64(clock, 0) == 11 && Markers.U64(clock, 8) == 22 &&
              Markers.U64(clock, 16) == 33 && Markers.U64(clock, 24) == 44, "clock payload offsets");
        var span = new byte[40];
        BinaryPrimitives.WriteUInt32LittleEndian(span.AsSpan(32), 34408);
        BinaryPrimitives.WriteUInt16LittleEndian(span.AsSpan(36), 2);
        Check(Markers.U32(span, 32) == 34408 && Markers.U16(span, 36) == 2, "span payload offsets");
    }

    private static void ClockMapping()
    {
        var epoch = new DateTime(2026, 9, 22, 14, 23, 50, DateTimeKind.Utc);
        var clocks = new[]
        {
            new ClockMarker(500, 500, 1_000_000, (ulong)epoch.ToFileTimeUtc(), 500),
            new ClockMarker(2_000_500, 2_000_500, 1_000_000,
                            (ulong)epoch.AddSeconds(2).ToFileTimeUtc(), 2_000_500),
        };
        Near((Markers.Utc(clocks, 1500) - epoch).TotalMicroseconds, 1000, "utc from the nearest clock", 1);
        Near((Markers.Utc(clocks, 2_001_500) - epoch).TotalMicroseconds, 2_001_000, "utc from the later clock", 1);
        Check(Markers.UtcText(Markers.Utc(clocks, 500)) == "2026-09-22T14:23:50.000000Z", "utc text");
    }

    private static int ClassId(string name) => Array.FindIndex(RvaTable.Classes, item => item.Name == name);

    private static bool Matches(string name, uint rva) => RvaTable.Classes[ClassId(name)].Contains(rva);

    private static void RvaClassTable()
    {
        Check(RvaTable.Classes.Length == 15, "every RVA class is defined");
        Check(RvaTable.Classes[ClassId("census_bracket")].Bound == 0x42B4420 + 0x1000,
              "the census bracket takes its assumed 0x1000 window");
        Check(!RvaTable.Classes[ClassId("census_bracket")].Pipeline,
              "the census bracket is matched but is not part of the pipeline");
        Check(RvaTable.BoundRule(RvaTable.Classes[ClassId("census_bracket")]) ==
              "entry+0x1000 (assumed, no decompile)", "the assumed window is named as assumed");
        Check(Matches("census_bracket", 0x42B4500) && !Matches("census_bracket", 0x42B5420),
              "census bracket window edges");
        // A function whose decompile note carries a size ends at entry+size; the
        // 0x2000 guess is only a fallback, and only table_ba0 still needs one.
        Check(RvaTable.Classes[ClassId("update_render_data_job")].Bound == 0x4321940 + 503,
              "update_render_data_job ends at its decompile size");
        Check(RvaTable.Classes[ClassId("render_data_batch")].Bound == 0x4320340 + 565,
              "render_data_batch ends at its decompile size");
        Check(RvaTable.Classes[ClassId("update_physics_objects_job")].Bound == 0x432B2A0 + 62,
              "update_physics_objects_job ends at its decompile size");
        Check(RvaTable.Classes[ClassId("eval_430efe0")].Bound == 0x430EFE0 + 1006,
              "eval ends at its decompile size");
        Check(RvaTable.Classes[ClassId("reset_repopulate")].Bound == 0x36A0F50 + 687,
              "reset_repopulate ends at its decompile size");
        Check(RvaTable.Classes[ClassId("record_drain")].Bound == 0x42DF940 + 134,
              "record_drain ends at its decompile size");
        Check(RvaTable.Classes[ClassId("lod_evaluator_4331300")].Bound == 0x4331300 + 9,
              "the LOD evaluator is a nine-byte jmp thunk");
        Check(Matches("lod_evaluator_4331300", 0x4331308) && !Matches("lod_evaluator_4331300", 0x4331309),
              "the LOD thunk matches nine bytes and no more");
        Check(RvaTable.BoundRule(RvaTable.Classes[ClassId("record_drain")]) == "decompile size (134 bytes)",
              "the decompile size is named in the bound rule");
        Check(RvaTable.Classes[ClassId("table_ba0")].Bound == 0x4321940,
              "table_ba0 has no decompile and keeps the next known entry");
        Check(RvaTable.BoundRule(RvaTable.Classes[ClassId("table_ba0")]) == "next known entry",
              "the fallback rule is named");
        // record_drain's 134 bytes still cover the 0x42df9a0 region the earlier
        // hand count saw under EDVR's d3d11 frames.
        Check(Matches("record_drain", 0x42DF9A0), "0x42df9a0 is inside record_drain");
        Check(!Matches("record_drain", 0x42DF9C6), "record_drain ends at 134 bytes");
        Check(!Matches("thunk_42df540", 0x42DF9A0), "0x42df9a0 is not the earlier thunk");
        Check(RvaTable.Classes[ClassId("thunk_42df540")].Bound == 0x42DF550, "a thunk takes 16 bytes");
        Check(Matches("thunk_42df540", 0x42DF54F) && !Matches("thunk_42df540", 0x42DF550),
              "thunk window edges");
        Check(Matches("reset_repopulate", 0x36A1000) && !Matches("reset_repopulate", 0x36A11FF),
              "reset_repopulate window edges");
        Check(!RvaTable.Classes[ClassId("reset_repopulate")].Pipeline,
              "reset_repopulate is matched but is not part of the pipeline");
        Check(RvaTable.Classes[ClassId("record_drain")].Pipeline, "record_drain is part of the pipeline");
        Check(Matches("call_site_431b21f", 0x431B21F) && Matches("call_site_431b21f", 0x431B22E) &&
              !Matches("call_site_431b21f", 0x431B22F), "call site matches only the 16 bytes after it");
        Check(Matches("scheduler_range", 0x5D6D7F) && Matches("scheduler_range", 0x5D6B20) &&
              !Matches("scheduler_range", 0x5D6ABD) && !Matches("scheduler_range", 0x5D7100),
              "scheduler range edges");
        Check(!RvaTable.Classes[ClassId("scheduler_range")].Pipeline, "the job scheduler is not the pipeline");
        Check(RvaTable.Classes.Count(item => item.Pipeline) == 12, "twelve classes make up the pipeline");
        foreach (var item in RvaTable.Classes) Check(item.Bound > item.Entry, $"{item.Name} has a positive window");
        Check(RvaTable.BoundRule(RvaTable.Classes[ClassId("thunk_42df540")]) == "entry+0x10 (thunk)",
              "the thunk bound rule is reported");
    }

    private static void StackLabels()
    {
        Check(StackStore.LabelFor(3, true, true, 3, false, false, false) == StackLabel.Pipeline,
              "pipeline outranks the scheduler");
        Check(StackStore.LabelFor(3, false, true, 3, false, false, false) == StackLabel.Scheduler,
              "scheduler outranks other game frames");
        Check(StackStore.LabelFor(3, false, false, 1, true, false, false) == StackLabel.GameOther,
              "a game frame outranks EDVR's d3d11");
        Check(StackStore.LabelFor(3, false, false, 0, true, false, true) == StackLabel.EdvrD3d11,
              "EDVR's d3d11 outranks other modules");
        Check(StackStore.LabelFor(3, false, false, 0, false, true, true) == StackLabel.EdvrOpenvrApi,
              "EDVR's openvr_api outranks other modules");
        Check(StackStore.LabelFor(3, false, false, 0, false, false, false) == StackLabel.KernelOnly,
              "kernel-only stack");
        Check(StackStore.LabelFor(0, false, false, 0, false, false, false) == StackLabel.Empty, "empty stack");
        Check(CycleAnalyzer.LabelName(StackLabel.Pipeline) == "pipeline", "label name");
        Check(Report.Site(new GameTop(0x5D6D7F, 0x5D4141, 0x5D67A6, 3)) == "0x5d6d7f < 0x5d4141 < 0x5d67a6",
              "blocking site text");
        Check(Report.Site(new GameTop(0, 0, 0, 0)) == "no_game_frame", "site without a game frame");
    }

    private static string WriteTemp(string name, string content)
    {
        var path = Path.Combine(Path.GetTempPath(), $"edvr-cpu-selftest-{Guid.NewGuid():N}-{name}");
        File.WriteAllText(path, content);
        return path;
    }

    private const string LogFixture =
        "2026-09-22 14:23:50.668 UTC pid=4242 tid=7 native_frame_cycle_window,window=30," +
        "boundary=host_wait_return_to_next_host_wait_return,admitted=3,valid=2,first=1,last=2," +
        "elapsed_ms=30000,valid_cycle_sum_ms=18.000,caller_thread=7,generation=1,feature_epoch=4\n" +
        "2026-09-22 14:23:50.668 UTC pid=4242 tid=7 native_frame_cycle_phase,window=30,name=cycle," +
        "mean=9.0000,p50=9.0000,p95=9.0000,units=wall_ms,nested=0\n" +
        "2026-09-22 14:23:50.668 UTC pid=4242 tid=7 native_frame_cycle_phase,window=30," +
        "name=game_before_first_submit,mean=2.0100,p50=2.0000,p95=2.0000,units=wall_ms,nested=0\n" +
        "2026-09-22 14:23:50.668 UTC pid=4242 tid=7 native_frame_cycle_phase,window=30," +
        "name=second_submit_owner_body,mean=3.0000,p50=3.0000,p95=3.0000,units=wall_ms,nested=1\n" +
        "2026-09-22 14:23:50.668 UTC pid=9999 tid=7 native_frame_cycle_window,window=44,admitted=1,valid=1," +
        "first=99,last=99\n";

    private static void RuntimeLogParsing()
    {
        var path = WriteTemp("runtime.log", LogFixture);
        try
        {
            var parsed = RuntimeLog.Parse(path, 4242);
            Check(parsed.Windows.Count == 1, "one window for this pid");
            Check(parsed.ForeignPidLines == 1, "a window from another pid is skipped");
            var window = parsed.Windows[0];
            Check(window.Window == 30 && window.First == 1 && window.Last == 2, "window identity");
            Check(window.Admitted == 3 && window.Valid == 2, "window admission counts");
            Check(window.Generation == 1 && window.FeatureEpoch == 4, "window shape");
            Check(window.CallerThread == 7 && window.ElapsedMs == 30000, "window thread and elapsed");
            Near(window.ValidCycleSumMs, 18.0, "window cycle sum");
            Near(window.Phases["cycle"].Mean, 9.0, "phase mean");
            Near(window.Phases["game_before_first_submit"].Mean, 2.01, "second phase mean");
            Check(window.Phases["second_submit_owner_body"].Nested, "nested phase flagged");
            Check(window.Phases["cycle"].Units == "wall_ms", "phase units");
            Check(window.EmittedUtc == new DateTime(2026, 9, 22, 14, 23, 50, 668, DateTimeKind.Utc),
                  "window emission timestamp");
            Check(RuntimeLog.PrimaryPhases.Length == 8, "the seven phases and the residual");
        }
        finally { File.Delete(path); }
    }

    // --- synthetic cycle fixture -------------------------------------------------
    // One caller thread (7) and one worker (11) of pid 4242. Frame 1 runs
    // 1000..10000 us with a blocked interval 4000..5200; frame 2 runs
    // 11000..20000 us entirely on the CPU.

    private const int Pid = 4242;
    private const int Caller = 7;
    private const int Worker = 11;
    private const ulong GameBase = 0x140000000;

    private static FrameMarker Frame(ulong offset, ulong sequence) => new(
        TimestampUs: 10000 + offset, Sequence: sequence, Generation: 1, FeatureEpoch: 4,
        WaitReturnUs: 1000 + offset, SecondSubmitReturnUs: 7000 + offset,
        NextWaitEntryUs: 9000 + offset, NextWaitReturnUs: 10000 + offset,
        PresentBeginUs: 7500 + offset, PresentEndUs: 8000 + offset,
        CallerThread: Caller, NextWaitThread: Caller, Status: 0,
        Flags: Markers.PostValid | Markers.SinglePresent, SceneReady: 1);

    private static Collected Fixture(bool boundaryWait = false)
    {
        var data = new Collected { HasCallStacks = true, EventsLost = 0 };
        var epoch = new DateTime(2026, 9, 22, 14, 23, 50, DateTimeKind.Utc);
        data.Clocks.Add(new ClockMarker(500, 500, 1_000_000, (ulong)epoch.ToFileTimeUtc(), 500));
        data.SampleIntervalUs = 1000;
        data.SampleIntervalSource = "self-test";
        data.CallerThreads.Add(Caller);
        data.KernelFirstQpc = 500;
        data.KernelLastQpc = 25000;
        data.CpuSwitchRanges[0] = new CpuSwitchRange { FirstQpc = 500, LastQpc = 25000, Count = 8 };
        data.SwitchOut = 4;
        data.SwitchIn = 4;

        foreach (var offset in new ulong[] { 0, 10000 })
        {
            data.Spans.Add(new SpanMarker(3200 + offset, 1, 3000 + offset, 0, Caller, 2, 0));
            data.Spans.Add(new SpanMarker(7010 + offset, 2, 3300 + offset, 0, Caller, 2, 0));
            data.Spans.Add(new SpanMarker(10020 + offset, 3, 8990 + offset, 0, Caller, 1, 0));
            data.Frames.Add(Frame(offset, offset == 0 ? 1ul : 5ul));
        }

        var caller = data.Timeline(Caller);
        caller.Add(900, CpuState.Running);
        if (boundaryWait)
        {
            // A wait that straddles the R1/R2 boundary at the first Submit entry.
            caller.Add(2900, CpuState.Waiting);
            caller.Add(3100, CpuState.Ready);
            caller.Add(3150, CpuState.Running);
        }
        caller.Add(4000, CpuState.Waiting);
        caller.Add(5000, CpuState.Ready);
        caller.Add(5200, CpuState.Running);
        caller.Add(9100, CpuState.Waiting);
        caller.Add(9800, CpuState.Ready);
        caller.Add(9900, CpuState.Running);
        var worker = data.Timeline(Worker);
        worker.Add(4200, CpuState.Running);
        worker.Add(5000, CpuState.Waiting);

        var stacks = new StackStore(@"c:\game");
        var exe = stacks.AddModuleForTest(@"c:\game\elitedangerous64.exe", "elitedangerous64.exe", GameBase);
        var edvrD3d11 = stacks.AddModuleForTest(@"c:\game\d3d11.dll", "d3d11.dll", 0x7FF000000000);
        var systemD3d11 = stacks.AddModuleForTest(@"c:\windows\system32\d3d11.dll", "d3d11.dll", 0x7FF100000000);
        var nvidia = stacks.AddModuleForTest(@"c:\windows\system32\driverstore\nvwgf2umx.dll",
                                             "nvwgf2umx.dll", 0x7FF200000000);
        stacks.AddModuleForTest(@"c:\windows\system32\ntdll.dll", "ntdll.dll", 0x7FF300000000);
        Check(stacks.DisplayName(edvrD3d11) == "d3d11.dll [edvr]", "EDVR's d3d11 is named apart");
        Check(stacks.DisplayName(systemD3d11) == "d3d11.dll [system32]", "System32's d3d11 is named apart");
        Check(stacks.DisplayName(nvidia) == "nvwgf2umx.dll", "the display driver is named");
        Check(stacks.DisplayName(-1) == "unresolved", "an unresolved frame is named as such");
        var pipeline = stacks.AddForTest(new StackInfo
        {
            ModuleIds = [exe, exe],
            Addresses = [GameBase + 0x42DF9A0, GameBase + 0x431B21F],
            ClassIds = [ClassId("record_drain"), ClassId("call_site_431b21f")],
            Label = StackLabel.Pipeline,
            Pipeline = true,
            GameTops = new GameTop(0x42DF9A0, 0x431B21F, 0, 2),
            Depth = 2,
            TopModuleId = exe,
        });
        var scheduler = stacks.AddForTest(new StackInfo
        {
            ModuleIds = [exe, exe, exe],
            Addresses = [GameBase + 0x5D6D7F, GameBase + 0x5D4141, GameBase + 0x5D67A6],
            ClassIds = [ClassId("scheduler_range")],
            Label = StackLabel.Scheduler,
            Scheduler = true,
            GameTops = new GameTop(0x5D6D7F, 0x5D4141, 0x5D67A6, 3),
            Depth = 3,
            TopModuleId = exe,
        });
        var other = stacks.AddForTest(new StackInfo
        {
            ModuleIds = [exe],
            Addresses = [GameBase + 0x1234],
            ClassIds = [],
            Label = StackLabel.GameOther,
            GameTops = new GameTop(0x1234, 0, 0, 1),
            Depth = 1,
            TopModuleId = exe,
        });
        var wakerReady = stacks.AddForTest(new StackInfo
        {
            ModuleIds = [exe],
            Addresses = [GameBase + 0x5D6DCD],
            ClassIds = [ClassId("scheduler_range")],
            Label = StackLabel.Scheduler,
            Scheduler = true,
            GameTops = new GameTop(0x5D6DCD, 0, 0, 1),
            Depth = 1,
            TopModuleId = exe,
        });
        // A draw submitted through EDVR's d3d11 into the display driver, with an
        // unresolved frame between them.
        var submit = stacks.AddForTest(new StackInfo
        {
            ModuleIds = [nvidia, -1, systemD3d11, edvrD3d11, exe],
            Addresses = [0x7FF200001000, 0x66600000, 0x7FF100002000, 0x7FF000003000, GameBase + 0x42B4500],
            ClassIds = [ClassId("census_bracket")],
            Label = StackLabel.OtherModule,
            Census = true,
            AnyEdvrD3d11 = true,
            AnySystemD3d11 = true,
            AnyNvidiaUserMode = true,
            AnyUnresolved = true,
            GameTops = new GameTop(0x42B4500, 0, 0, 1),
            Depth = 5,
            TopModuleId = nvidia,
        });
        Check(stacks.Signature(stacks[pipeline]) == "0x42df9a0 < 0x431b21f", "a game stack signs as its RVAs");
        Check(stacks.Signature(stacks[submit]) ==
              "nvwgf2umx.dll < unresolved < d3d11.dll [system32] < d3d11.dll [edvr] < 0x42b4500",
              "a mixed stack signs as modules and RVAs");
        Check(stacks.Signature(new StackInfo
        {
            ModuleIds = [4, 4, 4, exe],
            Addresses = [0x7FF300000100, 0x7FF300000200, 0x7FF300000300, GameBase + 0x10],
            Depth = 4,
        }) == "ntdll.dll*3 < 0x10", "a run in one module collapses");
        // Non-contiguous EDVR frames keep their order, the cap holds, and the
        // game call site is the first game frame below the LAST of them.
        var nested = new StackInfo
        {
            ModuleIds = [edvrD3d11, systemD3d11, edvrD3d11, exe, exe],
            Addresses = [0x7FF000001000, 0x7FF100000000, 0x7FF000002000, GameBase + 0x600,
                         GameBase + 0x700],
            Depth = 5,
        };
        var chain = stacks.EdvrFrames(nested, 6, out var site);
        Check(chain.Count == 2 && chain[0] == "0x1000" && chain[1] == "0x2000" && site == "0x600",
              "an EDVR chain is innermost outward with the entering game call site");
        Check(stacks.EdvrFrames(nested, 1, out _).Count == 1, "the chain cap holds");
        Check(stacks.EdvrFrames(stacks[other], 6, out var noSite).Count == 0 && noSite == "none",
              "a stack with no EDVR frame yields no chain");
        data.Stacks = stacks;

        Collected.Append(data.SamplesByThread, Caller, new StackObs(2000, pipeline));
        Collected.Append(data.SamplesByThread, Caller, new StackObs(6000, other));
        Collected.Append(data.SamplesByThread, Caller, new StackObs(6500, submit));
        Collected.Append(data.SamplesByThread, Worker, new StackObs(4500, pipeline));
        Collected.Append(data.SamplesByThread, Worker, new StackObs(4600, pipeline));
        Collected.Append(data.SwitchOutStacksByThread, Caller, new StackObs(4000, scheduler));
        Collected.Append(data.SwitchOutStacksByThread, Caller, new StackObs(9100, scheduler));
        Collected.Append(data.ReadiesByThread, Caller, new ReadyObs(5000, Worker, Pid, wakerReady));
        Collected.Append(data.ReadiesByThread, Caller, new ReadyObs(9800, 99, 12345, wakerReady));
        return data;
    }

    private static CycleAnalyzer Analyzer(Collected data) =>
        new(data, Pid, qpc => qpc, 500, 25000);

    private static void CycleDerivation()
    {
        var data = Fixture();
        var cycle = Analyzer(data).Analyze(data.Frames[0]);
        Check(cycle.Derived, "the two Submit spans were found");
        Check(cycle.FirstSubmitEntryUs == 3000 && cycle.FirstSubmitReturnUs == 3200 &&
              cycle.SecondSubmitEntryUs == 3300, "derived Submit timestamps");
        Near(cycle.CycleMs, 9.0, "cycle");
        Near(cycle.BeforeFirstMs, 2.0, "game_before_first_submit");
        Near(cycle.FirstSubmitMs, 0.2, "first_submit_roundtrip");
        Near(cycle.BetweenMs, 0.1, "between_eye_calls");
        Near(cycle.SecondSubmitMs, 3.7, "second_submit_roundtrip");
        Near(cycle.PostSubmitMs, 2.0, "post_second_submit_to_next_wait");
        Near(cycle.NextWaitMs, 1.0, "next_wait_roundtrip");
        Near(cycle.ResidualMs, 0.0, "the derived phases partition the cycle");
        Near(cycle.PhaseMs("cycle"), 9.0, "phase lookup by name");
        // The span brackets the runtime's own tick, so every offset is positive
        // and measured, not assumed.
        Check(cycle.SubmitOffsetKnown && cycle.WaitOffsetKnown, "span offsets measured");
        Near(cycle.SubmitSpanEndOffsetUs, 10, "second Submit span ends after the marker");
        Near(cycle.WaitSpanBeginOffsetUs, 10, "wait span begins before the marker");
        Near(cycle.WaitSpanEndOffsetUs, 20, "wait span ends after the marker");
        Check(Markers.UtcText(cycle.WaitReturnUtc) == "2026-09-22T14:23:50.000500Z", "wait return in UTC");

        // A frame whose Submit spans are missing is reported, not guessed at.
        var starved = new Collected { HasCallStacks = true };
        starved.Clocks.Add(data.Clocks[0]);
        starved.CallerThreads.Add(Caller);
        starved.Stacks = new StackStore();
        var blind = new CycleAnalyzer(starved, Pid, qpc => qpc, 500, 25000).Analyze(data.Frames[0]);
        Check(!blind.Derived && blind.DerivationReason == "no_submit_spans_on_caller_thread",
              "a frame without Submit spans is not derived");
        Check(!blind.Covered, "an underived frame is not counted as covered");
    }

    private static void CycleRegions()
    {
        var data = Fixture();
        var analyzer = Analyzer(data);
        var cycle = analyzer.Analyze(data.Frames[0]);
        Check(cycle.Covered, "the cycle lies inside scheduler coverage");
        Check(cycle.Regions.Length == 11, "eight regions and three coarse regions");
        foreach (var region in cycle.Regions)
            Near(region.States.Total, region.LengthUs, $"{region.Name} states sum to its length");
        foreach (var region in cycle.Regions) Near(region.States.Unknown, 0, $"{region.Name} has no unknown time");
        Near(cycle.Region("R1")!.States.Running, 2000, "R1 running");
        Near(cycle.Region("R2")!.States.Running, 200, "R2 running");
        Near(cycle.Region("R3")!.States.Running, 100, "R3 running");
        Near(cycle.Region("R4")!.States.Running, 2500, "R4 running");
        Near(cycle.Region("R4")!.States.Waiting, 1000, "R4 waiting");
        Near(cycle.Region("R4")!.States.Ready, 200, "R4 ready");
        Near(cycle.Region("R5a")!.States.Running, 500, "R5a running");
        Near(cycle.Region("R5b")!.States.Running, 500, "R5b running");
        Near(cycle.Region("R5c")!.States.Running, 1000, "R5c running");
        Near(cycle.Region("R6")!.States.Running, 200, "R6 running");
        Near(cycle.Region("R6")!.States.Waiting, 700, "R6 waiting");
        Near(cycle.Region("C1")!.LengthUs, cycle.Region("R1")!.LengthUs, "C1 is R1");
        Near(cycle.Region("C2")!.LengthUs, 5000, "C2 spans the first Submit to present end");
        Near(cycle.Region("C3")!.LengthUs, 1000, "C3 is the post-present window");
        Near(cycle.Region("R1")!.States.Running + cycle.Region("R2")!.States.Running +
             cycle.Region("R3")!.States.Running + cycle.Region("R4")!.States.Running +
             cycle.Region("R5a")!.States.Running + cycle.Region("R5b")!.States.Running +
             cycle.Region("R5c")!.States.Running + cycle.Region("R6")!.States.Running, 7000,
             "the regions partition the cycle's running time");
        Check(cycle.LegacyCovered && cycle.LegacyRegion is not null, "the legacy window is still reported");
        Near(cycle.LegacyRegion!.States.Running, 1000, "legacy window running time");
    }

    private static void WaitAttribution()
    {
        var data = Fixture();
        var cycle = Analyzer(data).Analyze(data.Frames[0]);
        Check(cycle.Waits.Count == 2, "both scheduler waits are in the cycle");
        var first = cycle.Waits[0];
        Near(first.LengthUs, 1000, "wait length");
        Check(first.EndedReady, "the wait ended at a ready event");
        Check(Report.Site(first.BlockingTops) == "0x5d6d7f < 0x5d4141 < 0x5d67a6", "blocking site");
        Check(first.WakerThread == Worker && first.WakerProcess == Pid, "waker identity");
        Check(Report.Site(first.WakerReadyTops) == "0x5d6dcd", "waker ready-stack top");
        // The waker's ready stack is inside the scheduler, so only its last sample
        // can say what it had just finished.
        Check(first.WakerClass == "pipeline", "waker classified by its last sample");
        Near(first.WakerSampleAgeUs, 400, "waker sample age");
        Check(cycle.Waits[1].WakerClass == "other_process", "a waker outside the process");
        Near(cycle.PipelineWaitUs, 1000, "wait time attributed to the pipeline");

        var stale = Fixture();
        stale.SamplesByThread[Worker].Clear();
        stale.SamplesByThread[Worker].Add(new StackObs(2000, 0));
        Check(Analyzer(stale).Analyze(stale.Frames[0]).Waits[0].WakerClass == "stale_sample",
              "a sample older than the window is not attribution");
        var unready = Fixture();
        unready.ReadiesByThread[Caller].Clear();
        Check(Analyzer(unready).Analyze(unready.Frames[0]).Waits[0].WakerClass == "no_ready_event",
              "a wait with no ready event has no waker");
        RegionSplit();
    }

    private static void RegionSplit()
    {
        var data = Fixture();
        var cycle = Analyzer(data).Analyze(data.Frames[0]);
        Check(CycleAnalyzer.PartitionRegionNames.Length == 8, "eight regions partition the cycle");
        for (var i = 0; i < CycleAnalyzer.PartitionRegionNames.Length; i++)
            Check(cycle.Regions[i].Name == CycleAnalyzer.PartitionRegionNames[i],
                  $"region {i} is {CycleAnalyzer.PartitionRegionNames[i]}");
        foreach (var segment in cycle.Waits)
            Near(segment.RegionUs.Sum(), segment.InsideUs, "a wait's region parts sum to its time in the cycle");
        // The 4000..5000 wait sits wholly inside R4, the second Submit roundtrip.
        Near(cycle.Waits[0].RegionUs[3], 1000, "the pacer wait lands in R4");
        Near(cycle.Waits[0].RegionUs[0], 0, "and not in R1");
        Near(cycle.Waits[1].RegionUs[7], 700, "the next-wait block lands in R6");

        var straddled = Fixture(true);
        var split = Analyzer(straddled).Analyze(straddled.Frames[0]);
        Check(split.Waits.Count == 3, "the boundary wait is a third segment");
        var straddle = split.Waits[0];
        Near(straddle.StartUs, 2900, "the boundary wait starts in R1");
        Near(straddle.RegionUs[0], 100, "clipped into R1");
        Near(straddle.RegionUs[1], 100, "clipped into R2");
        Near(straddle.RegionUs.Sum(), straddle.InsideUs, "the clipped parts still sum to the whole");
    }

    private static void ThreadBusyAndGate()
    {
        var data = Fixture();
        var analyzer = Analyzer(data);
        var cycle = analyzer.Analyze(data.Frames[0]);
        var caller = cycle.Threads.Single(thread => thread.ThreadId == Caller);
        var worker = cycle.Threads.Single(thread => thread.ThreadId == Worker);
        Near(caller.RunningUs, 7000, "caller running in the cycle");
        Near(worker.RunningUs, 800, "worker running in the cycle");
        Check(caller.Samples == 3 && caller.PipelineSamples == 1, "caller samples");
        Check(worker.Samples == 2 && worker.PipelineSamples == 2, "worker samples");
        Check(caller.ClassSamples![ClassId("record_drain")] == 1, "per-class sample counts per thread");
        Check(caller.ClassSamples![ClassId("call_site_431b21f")] == 1, "a stack counts in every class it hits");
        Near(cycle.CallerPipelineRunningUs, 1000, "caller pipeline running time");
        Near(cycle.ThreadPipelineRunningUs, 3000, "thread-summed pipeline running time");
        Near(cycle.CriticalPathShare, (1000.0 + 1000.0) / 3000.0, "critical path share");
        Check(analyzer.ClassSampleTotals[ClassId("record_drain")] == 3, "class totals across threads");
        Check(analyzer.ClassFirstSequence[ClassId("record_drain")] == 1, "first frame that sampled a class");
        Check(analyzer.ClassLastSequence[ClassId("record_drain")] == 1, "last frame that sampled a class");
        Check(analyzer.ClassSamplesByThread[Worker][ClassId("record_drain")] == 2, "per-thread class totals");

        CallerBreakdown(cycle, data);

        var second = analyzer.Analyze(data.Frames[1]);
        Near(second.Threads.Single(thread => thread.ThreadId == Caller).RunningUs, 9000,
             "an uninterrupted cycle is all running");
        Check(second.Waits.Count == 0, "an uninterrupted cycle has no waits");
        Near(second.CriticalPathShare, 0, "no pipeline samples means no share");
    }

    private static void CallerBreakdown(FrameCycle cycle, Collected data)
    {
        // Sample at 2000 is in R1 (first Submit entry is 3000); 6000 and 6500 are
        // in R2-R4 (the second Submit returns at 7000).
        Check(CycleAnalyzer.SampleRegionNames.Length == 4, "four coarse sample regions");
        Check(CycleAnalyzer.SampleRegion(cycle, 2999) == 0 && CycleAnalyzer.SampleRegion(cycle, 3000) == 1,
              "the R1 boundary is the first Submit entry");
        Check(CycleAnalyzer.SampleRegion(cycle, 6999) == 1 && CycleAnalyzer.SampleRegion(cycle, 7000) == 2,
              "the R2-R4 boundary is the second Submit return");
        Check(CycleAnalyzer.SampleRegion(cycle, 8999) == 2 && CycleAnalyzer.SampleRegion(cycle, 9000) == 3,
              "the R5 boundary is the next wait entry");
        Check(cycle.CallerSamplesByRegion.Count == 3, "every caller sample is bucketed");
        Check(cycle.CallerSamplesByRegion.Count(item => item.Group == 0) == 1, "one sample in R1");
        Check(cycle.CallerSamplesByRegion.Count(item => item.Group == 1) == 2, "two samples in R2-R4");

        var rows = (List<Dictionary<string, object?>>)Report.CallerStacksByRegion([cycle], data);
        Check(rows.Count == 4, "one row per coarse region");
        var r1 = rows[0];
        Check((string)r1["region"]! == "R1" && Convert.ToInt64(r1["samples"]) == 1, "R1 sample count");
        Near(Convert.ToDouble(r1["msPerFrame"]), 1.0, "one sample is one interval of a frame");
        var r1Modules = (Dictionary<string, object?>[])r1["byTopModule"]!;
        Check((string)r1Modules[0]["module"]! == "elitedangerous64.exe", "R1's top frame is game code");
        var r1Any = (Dictionary<string, object?>)r1["anyFrameIn"]!;
        Check(Convert.ToInt64(r1Any["pipelineClass"]) == 1 && Convert.ToInt64(r1Any["edvrD3d11"]) == 0,
              "R1's sample is pipeline and touches no runtime module");

        var submit = rows[1];
        var modules = (Dictionary<string, object?>[])submit["byTopModule"]!;
        Check(modules.Any(module => (string)module["module"]! == "nvwgf2umx.dll"),
              "a driver frame is named in the top-module table");
        var any = (Dictionary<string, object?>)submit["anyFrameIn"]!;
        // The five counts overlap: one stack carries all of them.
        Check(Convert.ToInt64(any["edvrD3d11"]) == 1 && Convert.ToInt64(any["systemD3d11"]) == 1 &&
              Convert.ToInt64(any["nvidiaUserMode"]) == 1 && Convert.ToInt64(any["censusBracket"]) == 1,
              "overlapping any-frame counts are independent");
        Check(Convert.ToInt64(any["anyFrameUnresolved"]) == 1 && Convert.ToInt64(any["topFrameUnresolved"]) == 0,
              "an unresolved frame inside a stack is counted without hiding the top");
        var innermost = (Dictionary<string, object?>[])submit["edvrInnermostRvas"]!;
        Check(innermost.Length == 1 && (string)innermost[0]["rva"]! == "0x3000",
              "the innermost EDVR frame is reported as an RVA of our own DLL");
        var chains = (Dictionary<string, object?>[])submit["edvrChains"]!;
        Check(chains.Length == 1 && ((string[])chains[0]["chain"]!)[0] == "0x3000" &&
              (string)chains[0]["gameCallSite"]! == "0x42b4500",
              "the EDVR chain carries the game call site that entered it");
        var stacksInRegion = (Dictionary<string, object?>[])submit["topStacks"]!;
        Check(stacksInRegion.Length == 2 && Convert.ToInt64(stacksInRegion[0]["count"]) == 1,
              "the region's stacks are grouped by signature");
        Check(rows[3] is not null && Convert.ToInt64(rows[3]["samples"]) == 0, "R6 has no samples here");
    }

    private static void WindowGrouping()
    {
        var data = Fixture();
        var logPath = WriteTemp("windows.log", LogFixture);
        var output = Path.Combine(Path.GetTempPath(), $"edvr-cpu-selftest-{Guid.NewGuid():N}");
        var input = WriteTemp("fixture.etl", "not a real trace, only its size is read");
        try
        {
            var parsed = RuntimeLog.Parse(logPath, Pid);
            var report = Report.Build(input, Pid, data, Path.Combine(output, "frames.jsonl"), parsed);
            var windows = (Dictionary<string, object?>[])report["windows"]!;
            Check(windows.Length == 1, "one window from the runtime log");
            Check(Convert.ToInt32(windows[0]["traceFrames"]) == 1,
                  "only the frame inside first..last belongs to the window");
            Check(Convert.ToInt32(windows[0]["logValid"]) == 2, "the log's own valid count is carried through");
            var phases = (List<Dictionary<string, object?>>)windows[0]["phases"]!;
            var cycle = phases.Single(phase => (string)phase["name"]! == "cycle");
            Near(Convert.ToDouble(cycle["traceMeanMs"]), 9.0, "window trace mean");
            Near(Convert.ToDouble(cycle["logMeanMs"]), 9.0, "window log mean");
            Near(Convert.ToDouble(cycle["deltaMs"]), 0.0, "window delta");
            var before = phases.Single(phase => (string)phase["name"]! == "game_before_first_submit");
            Near(Convert.ToDouble(before["deltaMs"]), -0.01, "a phase the log and the trace disagree on");

            // Without a runtime log the windows are fixed five-second bins of
            // waitReturn, anchored at the first frame.
            var wide = Fixture();
            const ulong late = 6_000_000;
            wide.Spans.Add(new SpanMarker(3200 + late, 4, 3000 + late, 0, Caller, 2, 0));
            wide.Spans.Add(new SpanMarker(7010 + late, 5, 3300 + late, 0, Caller, 2, 0));
            wide.Spans.Add(new SpanMarker(10020 + late, 6, 8990 + late, 0, Caller, 1, 0));
            wide.Frames.Add(Frame(late, 9));
            var unwindowed = Report.Build(input, Pid, wide, Path.Combine(output, "frames2.jsonl"), null);
            var fixedWindows = (Dictionary<string, object?>[])unwindowed["windows"]!;
            Check(fixedWindows.Length == 2, "fixed five-second windows without a runtime log");
            Check((string)fixedWindows[0]["source"]! == "fixed_5s", "window source is named");
            Check(Convert.ToInt32(fixedWindows[0]["traceFrames"]) == 2 &&
                  Convert.ToInt32(fixedWindows[1]["traceFrames"]) == 1, "frames fall in the right bin");
            Check(Convert.ToInt32(fixedWindows[1]["window"]) == 2, "the second bin is numbered two");
        }
        finally
        {
            File.Delete(logPath);
            File.Delete(input);
            if (Directory.Exists(output)) Directory.Delete(output, true);
        }
    }

    private static void ReportShape()
    {
        var data = Fixture();
        var output = Path.Combine(Path.GetTempPath(), $"edvr-cpu-selftest-{Guid.NewGuid():N}");
        var framesPath = Path.Combine(output, "frames.jsonl");
        var input = WriteTemp("fixture.etl", "not a real trace, only its size is read");
        try
        {
            var report = Report.Build(input, Pid, data, framesPath, null);
            // The keys tools/cpu_profile.py's smoke check reads must keep their
            // meaning: the legacy per-frame window is still [presentEnd, nextWaitEntry).
            foreach (var key in new[] { "schemaVersion", "coverageComplete", "analyzedFrameCount", "frames",
                                        "frameCount", "eventsLost", "topStacksInPostPresent",
                                        "topStacksInPostPresentByKind", "rawSwitchStates", "waitReasons",
                                        "perCpuSwitchRanges", "framesPath", "windows", "threads",
                                        "rvaClasses", "attributionSummary", "gate" })
                Check(report.ContainsKey(key), $"report key {key}");
            Check(Convert.ToInt32(report["schemaVersion"]) == 1, "schema version is unchanged");
            Check(Convert.ToInt32(report["analyzedFrameCount"]) == 2, "both frames are analyzed");
            var frames = (List<Dictionary<string, object?>>)report["frames"]!;
            Check(frames.Count == 2, "the legacy frames array is still in report.json");
            foreach (var key in new[] { "sequence", "startUs", "endUs", "runningUs", "waitingUs",
                                        "sampleStackCount", "switchOutStackCount", "readyWakerStackCount",
                                        "spanCount", "operations" })
                Check(frames[0].ContainsKey(key), $"legacy frame key {key}");
            Check(Convert.ToUInt64(frames[0]["startUs"]) == 8000, "legacy window start");
            Near(Convert.ToDouble(frames[0]["runningUs"]), 1000, "legacy window running time");
            Check((string)report["framesPath"]! == framesPath, "framesPath names the detail file");
            var lines = File.ReadAllLines(framesPath);
            Check(lines.Length == 2, "frames.jsonl carries one object per valid frame");
            Check(lines[0].Contains("\"firstSubmitEntryUs\":3000"), "frame detail carries derived timestamps");
            Check(lines[0].Contains("\"criticalPathShare\""), "frame detail carries the gate");
            Check(lines[0].Contains("\"blockingSite\":\"0x5d6d7f"), "frame detail carries the blocking site");
            var gate = (Dictionary<string, object?>)report["gate"]!;
            Near(Convert.ToDouble(gate["callerPipelineRunningUs"]), 1000, "gate caller pipeline running");
            Near(Convert.ToDouble(gate["callerPipelineWaitUs"]), 1000, "gate pipeline wait");
            Near(Convert.ToDouble(gate["threadPipelineRunningUs"]), 3000, "gate thread pipeline running");
            Near(Convert.ToDouble(gate["criticalPathShare"]), 0.6667, "gate critical path share", 0.0002);
            var attribution = (Dictionary<string, object?>)report["attributionSummary"]!;
            Check(Convert.ToInt32(attribution["waitSegments"]) == 2, "attribution counts both waits");
            Near(Convert.ToDouble(attribution["pipelineWaitShare"]), 1000.0 / 1700.0, "pipeline wait share", 0.001);
            Near(Convert.ToDouble(attribution["regionSplitResidualUs"]), 0, "the region split loses no time");
            var byRegion = (List<Dictionary<string, object?>>)attribution["byRegion"]!;
            Check(byRegion.Count == 8, "the region split covers every partition region");
            var r4 = byRegion.Single(row => (string)row["region"]! == "R4");
            Near(Convert.ToDouble(r4["waitUs"]), 1000, "R4 carries the pacer wait");
            Near(Convert.ToDouble(r4["waitUsPerFrame"]), 500, "R4 wait per frame over both cycles");
            var r4Classes = (Dictionary<string, object?>[])r4["byClass"]!;
            Check(r4Classes.Length == 1 && (string)r4Classes[0]["class"]! == "pipeline",
                  "R4's wait is attributed to a pipeline waker");
            var r6 = byRegion.Single(row => (string)row["region"]! == "R6");
            Check(((Dictionary<string, object?>[])r6["byClass"]!)[0]["class"] as string == "other_process",
                  "R6's wait was ended from outside the process");
            Near(Convert.ToDouble(byRegion.Single(row => (string)row["region"]! == "R1")["waitUs"]), 0,
                 "R1 has no wait in this fixture");
            var cross = (List<Dictionary<string, object?>>)attribution["byBlockingSiteAndWakerClass"]!;
            Check(cross.Count == 1, "one blocking site in this fixture");
            Check((string)cross[0]["site"]! == "0x5d6d7f < 0x5d4141 < 0x5d67a6", "the site is named by RVA");
            Check(Convert.ToInt32(cross[0]["segments"]) == 2, "both waits share the site");
            var crossClasses = (Dictionary<string, object?>[])cross[0]["byClass"]!;
            Check(crossClasses.Length == 2, "the site splits by waker class");
            Check((string)crossClasses[0]["class"]! == "pipeline" &&
                  Convert.ToDouble(crossClasses[0]["us"]) == 1000, "the larger class comes first");
            var threads = (Dictionary<string, object?>[])report["threads"]!;
            Check(threads.Length == 2, "the per-thread table lists both threads");
            Check(Convert.ToInt32(threads[0]["threadId"]) == Caller, "the busiest thread is the caller");
            Near(Convert.ToDouble(threads[0]["runningUs"]), 16000, "per-thread running time over both cycles");
            var rva = (Dictionary<string, object?>)report["rvaClasses"]!;
            var counts = (Dictionary<string, object?>)rva["sampleCountsByClass"]!;
            Check(Convert.ToInt64(counts["record_drain"]) == 3, "record_drain sample count");
            Check(Convert.ToInt64(counts["reset_repopulate"]) == 0, "reset_repopulate was never sampled here");
            var first = (Dictionary<string, object?>)rva["firstSampledFrameSequence"]!;
            var last = (Dictionary<string, object?>)rva["lastSampledFrameSequence"]!;
            Check(Convert.ToUInt64(first["record_drain"]) == 1 && Convert.ToUInt64(last["record_drain"]) == 1,
                  "first and last frame that sampled a class");
            Check(first["reset_repopulate"] is null && last["reset_repopulate"] is null,
                  "a class no thread sampled has no first or last frame");
            Check(Convert.ToInt32(report["cycleCoveredFrameCount"]) == 2, "both cycles are covered");
            Near(Convert.ToDouble(report["maxRegionResidualUs"]), 0, "regions leave no residual");
            var ownership = (Dictionary<string, object?>)report["stackOwnership"]!;
            Check(ownership.ContainsKey("readyStackOwnedByEmitter"), "ready-stack ownership is reported");
        }
        finally
        {
            File.Delete(input);
            if (Directory.Exists(output)) Directory.Delete(output, true);
        }
    }
}
