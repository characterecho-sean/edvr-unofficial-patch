using System.Globalization;

// The runtime aggregates the same timestamps into 30 s windows in its text log.
// Parsing those lines is what lets one flight answer a question twice: once from
// the marker stream and once from the runtime's own arithmetic, with the
// difference visible instead of assumed.

internal sealed record LogPhase(string Name, double Mean, double P50, double P95, string Units, bool Nested);

internal sealed class LogWindow
{
    public int Window;
    public ulong First;
    public ulong Last;
    public long Admitted;
    public long Valid;
    public long ElapsedMs;
    public ulong Generation;
    public ulong FeatureEpoch;
    public int CallerThread;
    public double ValidCycleSumMs;
    public DateTime EmittedUtc;
    public string Boundary = "";
    public readonly Dictionary<string, LogPhase> Phases = new(StringComparer.Ordinal);
}

internal sealed class RuntimeLogResult
{
    public readonly List<LogWindow> Windows = [];
    public int WindowLines;
    public int PhaseLines;
    public int UnparsedLines;
    public int ForeignPidLines;
    public string Path = "";
}

internal static class RuntimeLog
{
    private const string WindowTag = "native_frame_cycle_window,";
    private const string PhaseTag = "native_frame_cycle_phase,";

    /// The seven phases FrameCycleStats reports as a partition of one cycle, in
    /// the order the cycle runs. per_frame_residual is the runtime's own check
    /// that they sum to the cycle.
    public static readonly string[] PrimaryPhases =
    [
        "cycle", "game_before_first_submit", "first_submit_roundtrip", "between_eye_calls",
        "second_submit_roundtrip", "post_second_submit_to_next_wait", "next_wait_roundtrip",
        "per_frame_residual",
    ];

    public static RuntimeLogResult Parse(string path, int pid)
    {
        var result = new RuntimeLogResult { Path = path };
        var byWindow = new Dictionary<int, LogWindow>();
        foreach (var line in File.ReadLines(path))
        {
            var isWindow = line.Contains(WindowTag, StringComparison.Ordinal);
            var isPhase = !isWindow && line.Contains(PhaseTag, StringComparison.Ordinal);
            if (!isWindow && !isPhase) continue;
            if (pid > 0 && !line.Contains($"pid={pid} ", StringComparison.Ordinal))
            {
                result.ForeignPidLines++;
                continue;
            }
            var tag = isWindow ? WindowTag : PhaseTag;
            var fields = Fields(line[(line.IndexOf(tag, StringComparison.Ordinal) + tag.Length)..]);
            if (!fields.TryGetValue("window", out var windowText) ||
                !int.TryParse(windowText, out var windowNumber))
            {
                result.UnparsedLines++;
                continue;
            }
            if (!byWindow.TryGetValue(windowNumber, out var window))
            {
                byWindow[windowNumber] = window = new LogWindow { Window = windowNumber };
                result.Windows.Add(window);
            }
            if (isWindow)
            {
                result.WindowLines++;
                window.EmittedUtc = Timestamp(line);
                window.First = ULong(fields, "first");
                window.Last = ULong(fields, "last");
                window.Admitted = (long)ULong(fields, "admitted");
                window.Valid = (long)ULong(fields, "valid");
                window.ElapsedMs = (long)ULong(fields, "elapsed_ms");
                window.Generation = ULong(fields, "generation");
                window.FeatureEpoch = ULong(fields, "feature_epoch");
                window.CallerThread = (int)ULong(fields, "caller_thread");
                window.ValidCycleSumMs = Double(fields, "valid_cycle_sum_ms");
                window.Boundary = fields.GetValueOrDefault("boundary", "");
            }
            else
            {
                result.PhaseLines++;
                var name = fields.GetValueOrDefault("name", "");
                if (name.Length == 0) { result.UnparsedLines++; continue; }
                window.Phases[name] = new LogPhase(name, Double(fields, "mean"), Double(fields, "p50"),
                    Double(fields, "p95"), fields.GetValueOrDefault("units", ""),
                    fields.GetValueOrDefault("nested", "0") == "1");
            }
        }
        result.Windows.Sort((a, b) => a.Window.CompareTo(b.Window));
        return result;
    }

    private static Dictionary<string, string> Fields(string text)
    {
        var fields = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var item in text.Split(','))
        {
            var split = item.IndexOf('=');
            if (split <= 0) continue;
            fields[item[..split].Trim()] = item[(split + 1)..].Trim();
        }
        return fields;
    }

    private static ulong ULong(Dictionary<string, string> fields, string key) =>
        fields.TryGetValue(key, out var text) && ulong.TryParse(text, out var value) ? value : 0;

    private static double Double(Dictionary<string, string> fields, string key) =>
        fields.TryGetValue(key, out var text) &&
        double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var value) ? value : 0;

    /// "2026-09-22 14:23:50.668 UTC pid=... " — the line's own emission time.
    public static DateTime Timestamp(string line)
    {
        if (line.Length < 23) return DateTime.MinValue;
        return DateTime.TryParseExact(line[..23], "yyyy-MM-dd HH:mm:ss.fff", CultureInfo.InvariantCulture,
                                      DateTimeStyles.AssumeUniversal | DateTimeStyles.AdjustToUniversal,
                                      out var value) ? value : DateTime.MinValue;
    }
}
