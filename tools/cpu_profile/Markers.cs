using System.Buffers.Binary;
using Microsoft.Diagnostics.Tracing;

// The marker payloads carry raw QPC ticks on purpose, so the header's QPC is
// read directly rather than through the relative-millisecond view TraceEvent
// prefers for ordinary analysis.
#pragma warning disable CS0618

// The manifest-free payload contract lives in src\common\native_cpu_trace_events.h.
// Offsets here are byte offsets in that header's packed structs; a payload whose
// length does not match is counted, never guessed at.

internal sealed record ClockMarker(ulong TimestampUs, ulong QpcTicks, ulong QpcFrequency,
                                   ulong SystemTime100ns, long HeaderQpc);

internal sealed record SpanMarker(ulong TimestampUs, ulong CallId, ulong BeginUs, long Result,
                                  uint Thread, ushort Operation, ushort Flags);

internal sealed record FrameMarker(ulong TimestampUs, ulong Sequence, ulong Generation,
                                   ulong FeatureEpoch, ulong WaitReturnUs, ulong SecondSubmitReturnUs,
                                   ulong NextWaitEntryUs, ulong NextWaitReturnUs, ulong PresentBeginUs,
                                   ulong PresentEndUs, uint CallerThread, uint NextWaitThread,
                                   ushort Status, ushort Flags, uint SceneReady);

internal static class Markers
{
    public const ushort Version = 1;
    public const ushort PostValid = 1 << 0;
    public const ushort SinglePresent = 1 << 1;
    public const ushort SubmitOperation = 2;
    public const ushort WaitOperation = 1;

    public static void Parse(TraceEvent data, Collected result)
    {
        if (data.Version != Version)
        {
            result.UnsupportedMarkers++;
            AddError(result, $"event {data.ID}: unsupported version {data.Version}");
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
                    AddError(result, $"event {data.ID}: payload length {bytes.Length}");
                    break;
                default:
                    result.OtherMarkerEvents++;
                    break;
            }
        }
        catch (Exception ex)
        {
            result.MalformedMarkers++;
            AddError(result, $"event {data.ID}: {ex.Message}");
        }
    }

    public static void AddError(Collected result, string error)
    {
        if (result.MarkerErrors.Count < 20) result.MarkerErrors.Add(error);
    }

    public static ulong U64(byte[] b, int o) => BinaryPrimitives.ReadUInt64LittleEndian(b.AsSpan(o, 8));
    public static long I64(byte[] b, int o) => BinaryPrimitives.ReadInt64LittleEndian(b.AsSpan(o, 8));
    public static uint U32(byte[] b, int o) => BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(o, 4));
    public static ushort U16(byte[] b, int o) => BinaryPrimitives.ReadUInt16LittleEndian(b.AsSpan(o, 2));

    public static string? FrameInvalidReason(FrameMarker frame)
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

    public static string FrameStatusName(ushort status) => status switch
    {
        1 => "provider_missing", 2 => "bad_provider_version", 3 => "bad_provider_size",
        4 => "bad_provider_generation", 5 => "not_yet_observable", 6 => "lost_present_history",
        7 => "partial_present", 8 => "wrong_present_thread", 9 => "failed_present",
        10 => "test_present", 11 => "malformed_present", _ => $"unknown_{status}",
    };

    public static string OperationName(ushort operation) => operation switch
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

    public sealed record SequenceResult(int Gaps, int Duplicates);

    public static SequenceResult SequenceHealth(IEnumerable<FrameMarker> frames)
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

    /// The QPC-to-UTC mapping, taken from the clock marker nearest the timestamp.
    /// systemTime100ns is a FILETIME, so the offset is applied in microseconds.
    public static DateTime Utc(ClockMarker[] clocks, ulong timestampUs)
    {
        if (clocks.Length == 0) return DateTime.MinValue;
        var index = Numeric.LowerBound(clocks.Length, i => clocks[i].TimestampUs > timestampUs);
        var marker = clocks[Math.Max(0, index - 1)];
        var deltaUs = (double)timestampUs - marker.TimestampUs;
        return DateTime.FromFileTimeUtc((long)marker.SystemTime100ns).AddTicks((long)(deltaUs * 10.0));
    }

    public static string UtcText(DateTime value) =>
        value == DateTime.MinValue ? "" : value.ToString("yyyy-MM-ddTHH:mm:ss.ffffffZ");
}
