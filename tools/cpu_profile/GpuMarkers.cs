internal sealed record GpuCompletionMarker(ulong TimestampUs, ulong PublicationQpc,
    ulong Sequence, ulong SourceFrame, ulong AgeMs, double OuterMs, ushort Source,
    ushort Reason, long HeaderQpc);

// Event 4 is a completion PUBLICATION witness. It never supplies GPU execution
// start/end times, and packet/provider presence is not used to manufacture one.
internal static class GpuMarkers
{
    public const string TimestampMeaning = "CPU publication QPC/us, not GPU execution timestamps";
    public static void Parse(int version, byte[] bytes, long headerQpc, Collected data)
    {
        if (version != 1 || bytes.Length != 56 ||
            Markers.U16(bytes, 52) != 1 || Markers.U16(bytes, 54) != 56)
        {
            data.GpuMarkerSchemaErrors++;
            Markers.AddError(data, $"GPU completion: unsupported version or payload size ({version}/{bytes.Length})");
            return;
        }
        data.GpuCompletions.Add(new(Markers.U64(bytes, 0), Markers.U64(bytes, 8),
            Markers.U64(bytes, 16), Markers.U64(bytes, 24), Markers.U64(bytes, 32),
            BitConverter.Int64BitsToDouble(Markers.I64(bytes, 40)),
            Markers.U16(bytes, 48), Markers.U16(bytes, 50), headerQpc));
    }

    public static string? Invalid(GpuCompletionMarker marker) => marker switch
    {
        { TimestampUs: 0 } or { PublicationQpc: 0 } or { Sequence: 0 } => "zero_identity_or_timestamp",
        { Source: > 1 } => "unknown_source",
        { Reason: > 13 } => "unknown_reason",
        _ when !double.IsFinite(marker.OuterMs) || marker.OuterMs < 0 => "bad_duration",
        { Reason: 0, SourceFrame: 0 } => "zero_source_frame",
        { Reason: 0, AgeMs: > 2000 } => "stale_valid_result",
        _ => null,
    };

    public static Dictionary<string, object?> Unavailable(string status) => new()
    {
        ["status"] = status, ["valid"] = false, ["ms"] = null,
        ["timestampMeaning"] = TimestampMeaning,
    };

    private static string? ClockInvalid(GpuCompletionMarker marker, Collected data)
    {
        var frequencies = data.Clocks.Select(c => c.QpcFrequency).Distinct().ToArray();
        if (frequencies.Length != 1 || frequencies[0] == 0) return "publication_clock_unavailable";
        return Math.Abs(marker.TimestampUs - marker.PublicationQpc * 1_000_000.0 / frequencies[0]) > 2
            ? "publication_clock_mismatch" : null;
    }

    public static Dictionary<string, object?> Join(Collected data, List<FrameCycle> cycles)
    {
        // XR frame sequence and the graphics producer counter are independent.
        // ONLY a V2 completed-frame witness maps its own retained producer ID.
        // Use all mappings, including unavailable CPU cycles: reuse/duplicates
        // across generations must never guess which frame owns a GPU result.
        var cpu = data.Frames.Where(f => f.SchemaVersion == 2 && f.GpuSequence != 0)
            .GroupBy(f => f.GpuSequence).ToDictionary(g => g.Key, g => g.Count());
        var cpuIdentity = data.Frames.GroupBy(f => (f.Generation, f.FeatureEpoch, f.Sequence))
            .ToDictionary(g => g.Key, g => g.Count());
        var gpu = data.GpuCompletions.GroupBy(f => f.Sequence).ToDictionary(g => g.Key, g => g.ToArray());
        var statuses = new Dictionary<string, long>();
        var invalidMarkers = data.GpuCompletions.Count(g => Invalid(g) is not null);
        foreach (var cycle in cycles)
        {
            var sequence = cycle.Marker.GpuSequence;
            var status = data.EventsLost != 0 ? "trace_events_lost" :
                data.GpuMarkerSchemaErrors != 0 ? "gpu_marker_schema_errors" :
                cycle.Marker.SchemaVersion != 2 ? "cpu_mapping_unavailable_v1" :
                sequence == 0 ? "cpu_producer_unavailable" :
                data.GpuCompletions.Count == 0 ? "markers_unavailable" :
                cpuIdentity[(cycle.Marker.Generation, cycle.Marker.FeatureEpoch, cycle.Marker.Sequence)] != 1 ? "ambiguous_cpu_sequence" :
                !cpu.TryGetValue(sequence, out var count) || count != 1 ? "ambiguous_cpu_sequence" :
                !gpu.TryGetValue(sequence, out var matches) ? "missing_sequence" :
                matches.Length != 1 ? "duplicate_gpu_sequence" : "candidate";
            cycle.ApplicationGpu = Unavailable(status);
            cycle.ApplicationGpu["producerSequence"] = sequence == 0 ? null : sequence;
            if (status == "candidate")
            {
                var marker = gpu[sequence][0];
                status = Invalid(marker) ?? ClockInvalid(marker, data) ?? (marker.Source != 1 ? "render_to_submit_only" :
                    marker.Reason != 0 ? "gpu_result_unavailable" : "valid");
                cycle.ApplicationGpu = Unavailable(status);
                cycle.ApplicationGpu["producerSequence"] = sequence;
                cycle.ApplicationGpu["source"] = marker.Source;
                cycle.ApplicationGpu["reason"] = marker.Reason;
                cycle.ApplicationGpu["sourceFrame"] = marker.SourceFrame;
                cycle.ApplicationGpu["ageMs"] = marker.AgeMs;
                cycle.ApplicationGpu["publicationUs"] = marker.TimestampUs;
                cycle.ApplicationGpu["publicationQpc"] = marker.PublicationQpc;
                cycle.ApplicationGpu["headerQpc"] = marker.HeaderQpc;
                if (status == "valid")
                {
                    cycle.ApplicationGpu["valid"] = true;
                    cycle.ApplicationGpu["ms"] = marker.OuterMs;
                }
            }
            Numeric.Increment(statuses, status);
        }
        return new()
        {
            ["status"] = data.GpuCompletions.Count == 0 && data.GpuMarkerSchemaErrors == 0
                ? "markers_unavailable" : "completion_witnesses_observed",
            ["markerCount"] = data.GpuCompletions.Count,
            ["schemaErrors"] = data.GpuMarkerSchemaErrors,
            ["invalidMarkers"] = invalidMarkers,
            ["duplicateGpuSequences"] = gpu.Count(g => g.Value.Length != 1),
            ["ambiguousCpuSequences"] = cpu.Count(c => c.Value != 1),
            ["explicitCpuMappings"] = data.Frames.Count(f => f.SchemaVersion == 2 && f.GpuSequence != 0),
            ["legacyCpuFrames"] = data.Frames.Count(f => f.SchemaVersion != 2),
            ["unavailableCpuMappings"] = data.Frames.Count(f => f.SchemaVersion == 2 && f.GpuSequence == 0),
            ["gpuSequencesWithoutCpuWitness"] = gpu.Count(g => !cpu.ContainsKey(g.Key)),
            ["cycleStatuses"] = statuses,
            ["eventsLost"] = data.EventsLost,
            ["timestampMeaning"] = TimestampMeaning,
            ["join"] = "explicit completed-frame V2 producer sequence only; XR/sourceFrame counters are never inferred",
        };
    }
}
