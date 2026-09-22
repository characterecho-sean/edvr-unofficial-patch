using System.Diagnostics;
using System.Text.Json;
using Microsoft.Diagnostics.Tracing;
using Microsoft.Diagnostics.Tracing.Etlx;

// EDVR CPU profile analyzer. Reads one ETW trace plus, optionally, the runtime's
// own text log, and answers where a frame cycle's CPU time went: per region of
// the cycle, per thread, and per scheduler wait with the waker that ended it.
//
//   dotnet EdvrCpuProfile.dll --input <etl> --pid <pid> --output <report.json>
//                             [--runtime-log <edvr_openxr_*.log>] [--frames <frames.jsonl>]
//
// report.json keeps every key it has always carried; the per-frame detail is
// written to frames.jsonl beside it, named by report.json's framesPath.

internal static class Program
{
    public static int Main(string[] args)
    {
        try
        {
            if (args.Length == 1 && args[0] == "--self-test")
            {
                SelfTests.Run();
                Console.WriteLine("EdvrCpuProfile self-test passed");
                return 0;
            }

            var options = ParseOptions(args);
            var watch = Stopwatch.StartNew();
            var report = Analyze(options, message => Console.WriteLine(
                $"[{watch.Elapsed.TotalSeconds,7:F1}s] {message}"));
            using var self = Process.GetCurrentProcess();
            var peakMb = self.PeakWorkingSet64 / 1048576.0;
            report["analyzerWallSeconds"] = Numeric.Round(watch.Elapsed.TotalSeconds);
            report["analyzerPeakWorkingSetMb"] = Numeric.Round(peakMb);
            var jsonOptions = new JsonSerializerOptions { WriteIndented = true };
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(options.Output))!);
            File.WriteAllText(options.Output, JsonSerializer.Serialize(report, jsonOptions));
            Console.WriteLine($"EDVR CPU report: {options.Output}");
            Console.WriteLine($"frames={report["frameCount"]} analyzed={report["analyzedFrameCount"]} " +
                              $"eventsLost={report["eventsLost"]} coverageComplete={report["coverageComplete"]}");
            Console.WriteLine($"cycleDerived={report["cycleDerivedFrameCount"]} " +
                              $"cycleCovered={report["cycleCoveredFrameCount"]} " +
                              $"frameDetail={report["framesPath"]}");
            Console.WriteLine($"wall={report["analyzerWallSeconds"]}s peakWorkingSet=" +
                              $"{report["analyzerPeakWorkingSetMb"]}MB");
            return (bool)report["coverageComplete"]! ? 0 : 2;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"EdvrCpuProfile: {ex.Message}");
            return 1;
        }
    }

    internal sealed record Options(string Input, int Pid, string Output, string? RuntimeLog, string Frames);

    private const string Usage = "usage: EdvrCpuProfile --input <etl> --pid <pid> --output <report.json> " +
                                 "[--runtime-log <log>] [--frames <frames.jsonl>] | --self-test";

    internal static Options ParseOptions(string[] args)
    {
        string? input = null, output = null, runtimeLog = null, frames = null;
        int? pid = null;
        for (var i = 0; i < args.Length; i++)
        {
            if (args[i] == "--input" && ++i < args.Length) input = args[i];
            else if (args[i] == "--pid" && ++i < args.Length && int.TryParse(args[i], out var parsed)) pid = parsed;
            else if (args[i] == "--output" && ++i < args.Length) output = args[i];
            else if (args[i] == "--runtime-log" && ++i < args.Length) runtimeLog = args[i];
            else if (args[i] == "--frames" && ++i < args.Length) frames = args[i];
            else throw new ArgumentException(Usage);
        }
        if (string.IsNullOrWhiteSpace(input) || string.IsNullOrWhiteSpace(output) || pid is null || pid <= 0)
            throw new ArgumentException(Usage);
        if (!File.Exists(input)) throw new FileNotFoundException("ETL input does not exist", input);
        if (runtimeLog is not null && !File.Exists(runtimeLog))
            throw new FileNotFoundException("runtime log does not exist", runtimeLog);
        var outputPath = Path.GetFullPath(output);
        var framesPath = string.IsNullOrWhiteSpace(frames)
            ? Path.Combine(Path.GetDirectoryName(outputPath)!,
                           Path.GetFileNameWithoutExtension(outputPath) + ".frames.jsonl")
            : Path.GetFullPath(frames);
        return new Options(Path.GetFullPath(input), pid.Value, outputPath,
                           runtimeLog is null ? null : Path.GetFullPath(runtimeLog), framesPath);
    }

    private static Dictionary<string, object?> Analyze(Options options, Action<string>? progress)
    {
        var runtimeLog = options.RuntimeLog is null ? null : RuntimeLog.Parse(options.RuntimeLog, options.Pid);
        if (runtimeLog is not null)
            progress?.Invoke($"runtime log: windows={runtimeLog.Windows.Count} " +
                             $"phaseLines={runtimeLog.PhaseLines} foreignPid={runtimeLog.ForeignPidLines}");
        var tempEtlx = Path.Combine(Path.GetTempPath(), $"edvr-cpu-{Guid.NewGuid():N}.etlx");
        try
        {
            var converted = TraceLog.CreateFromEventTraceLogFile(options.Input, tempEtlx, new TraceLogOptions(),
                                                                  new TraceEventDispatcherOptions());
            progress?.Invoke($"etlx: {new FileInfo(converted).Length / 1048576} MB");
            using var log = new TraceLog(converted);
            var data = Collector.Collect(log, options.Pid, progress);
            return Report.Build(options.Input, options.Pid, data, options.Frames, runtimeLog, progress);
        }
        finally
        {
            try { if (File.Exists(tempEtlx)) File.Delete(tempEtlx); }
            catch { /* A report is more useful than failing because a temporary ETLX stayed open. */ }
        }
    }
}
