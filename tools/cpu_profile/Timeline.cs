// Per-thread scheduler timelines and the distribution helpers the report shares.
// The runtime aggregates the same phases in src\openxr\frame_cycle_stats.h; the
// percentile rule here is deliberately the same integer rule, so a trace
// percentile and a log percentile are the same statistic.

internal enum CpuState : byte { BoundaryUnknown = 0, UnexpectedUnknown = 1, Running = 2, Ready = 3, Waiting = 4 }

/// A thread's scheduler transitions, appended in trace order. ETLX events arrive
/// sorted by timestamp, so the arrays stay sorted by (QPC, arrival) without an
/// explicit ordering key; ties keep the order the scheduler emitted them in.
internal sealed class ThreadTimeline
{
    private long[] _qpc = new long[4];
    private byte[] _state = new byte[4];
    public int Count { get; private set; }

    public void Add(long qpc, CpuState state)
    {
        if (Count == _qpc.Length)
        {
            Array.Resize(ref _qpc, Count * 2);
            Array.Resize(ref _state, Count * 2);
        }
        _qpc[Count] = qpc;
        _state[Count] = (byte)state;
        Count++;
    }

    public long QpcAt(int index) => _qpc[index];
    public CpuState StateAt(int index) => (CpuState)_state[index];

    /// True when every appended QPC is non-decreasing, which the report asserts
    /// instead of sorting millions of transitions a second time.
    public bool Sorted()
    {
        for (var i = 1; i < Count; i++) if (_qpc[i] < _qpc[i - 1]) return false;
        return true;
    }

    public int FirstIndexAfter(double startUs, Func<long, double> qpcUs) =>
        Numeric.LowerBound(Count, index => qpcUs(_qpc[index]) > startUs);
}

internal struct StateDurations
{
    public double Running, Ready, Waiting, BoundaryUnknown, UnexpectedUnknown;
    public double Unknown => BoundaryUnknown + UnexpectedUnknown;
    public double Total => Running + Ready + Waiting + BoundaryUnknown + UnexpectedUnknown;

    public void Add(CpuState state, double duration)
    {
        switch (state)
        {
            case CpuState.Running: Running += duration; break;
            case CpuState.Ready: Ready += duration; break;
            case CpuState.Waiting: Waiting += duration; break;
            case CpuState.UnexpectedUnknown: UnexpectedUnknown += duration; break;
            default: BoundaryUnknown += duration; break;
        }
    }
}

internal readonly record struct Dist(int Count, double Mean, double P50, double P95, double Min, double Max)
{
    public static Dist Of(List<double> values)
    {
        if (values.Count == 0) return new Dist(0, 0, 0, 0, 0, 0);
        var sorted = values.ToArray();
        Array.Sort(sorted);
        var total = 0.0;
        foreach (var value in sorted) total += value;
        return new Dist(sorted.Length, total / sorted.Length, Numeric.Percentile(sorted, 50),
                        Numeric.Percentile(sorted, 95), sorted[0], sorted[^1]);
    }
}

internal static class Numeric
{
    public static int LowerBound(int count, Func<int, bool> predicate)
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

    /// The integer percentile rule FrameCycleStats::dist uses.
    public static double Percentile(double[] sorted, int percent)
    {
        if (sorted.Length == 0) return 0;
        var index = (sorted.Length * percent + 99) / 100 - 1;
        return sorted[Math.Clamp(index, 0, sorted.Length - 1)];
    }

    public static StateDurations Durations(ThreadTimeline? timeline, double start, double end,
                                          Func<long, double> qpcUs)
    {
        var values = new StateDurations();
        if (end <= start) return values;
        if (timeline is null || timeline.Count == 0)
        {
            values.BoundaryUnknown = end - start;
            return values;
        }
        var state = CpuState.BoundaryUnknown;
        var cursor = start;
        var index = timeline.FirstIndexAfter(start, qpcUs);
        if (index > 0) state = timeline.StateAt(index - 1);
        while (index < timeline.Count)
        {
            var next = qpcUs(timeline.QpcAt(index));
            if (next >= end) break;
            if (next > cursor) values.Add(state, next - cursor);
            cursor = Math.Max(cursor, next);
            state = timeline.StateAt(index++);
        }
        if (cursor < end) values.Add(state, end - cursor);
        return values;
    }

    /// Running microseconds only, for the per-thread busy table: the same walk as
    /// Durations with the other states dropped, which keeps the loop over every
    /// thread of the process cheap.
    public static double RunningUs(ThreadTimeline? timeline, double start, double end,
                                   Func<long, double> qpcUs)
    {
        if (end <= start || timeline is null || timeline.Count == 0) return 0;
        var running = 0.0;
        var index = timeline.FirstIndexAfter(start, qpcUs);
        var state = index > 0 ? timeline.StateAt(index - 1) : CpuState.BoundaryUnknown;
        var cursor = start;
        while (index < timeline.Count)
        {
            var next = qpcUs(timeline.QpcAt(index));
            if (next >= end) break;
            if (state == CpuState.Running && next > cursor) running += next - cursor;
            cursor = Math.Max(cursor, next);
            state = timeline.StateAt(index++);
        }
        if (state == CpuState.Running && cursor < end) running += end - cursor;
        return running;
    }

    public static double UnionDuration(IEnumerable<(double Start, double End)> ranges)
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

    public static double Round(double value) => Math.Round(value, 3, MidpointRounding.AwayFromZero);
    public static double Round4(double value) => Math.Round(value, 4, MidpointRounding.AwayFromZero);

    public static void Increment<T>(Dictionary<T, long> counts, T key) where T : notnull =>
        counts[key] = counts.GetValueOrDefault(key) + 1;

    public static void Add(Dictionary<string, double> totals, string key, double value) =>
        totals[key] = totals.GetValueOrDefault(key) + value;

    public static Dictionary<string, object?> Json(Dist dist) => new()
    {
        ["count"] = dist.Count,
        ["mean"] = Round(dist.Mean),
        ["p50"] = Round(dist.P50),
        ["p95"] = Round(dist.P95),
        ["min"] = Round(dist.Min),
        ["max"] = Round(dist.Max),
    };

    public static Dictionary<string, object?> JsonMs(Dist dist) => new()
    {
        ["count"] = dist.Count,
        ["mean"] = Round4(dist.Mean),
        ["p50"] = Round4(dist.P50),
        ["p95"] = Round4(dist.P95),
        ["min"] = Round4(dist.Min),
        ["max"] = Round4(dist.Max),
    };
}
