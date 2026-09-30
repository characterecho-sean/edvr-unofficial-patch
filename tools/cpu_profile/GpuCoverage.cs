// Provider presence only: no decoding of driver-version-dependent GPU payloads,
// no busy-time estimate, and no assumption that a kernel event's PID owns its GPU context.
internal sealed class GpuCoverage
{
    public static readonly Guid DxgKrnl = new("802ec45a-1e99-4b83-9920-87c98277ba9d");
    public static readonly Guid Direct3D11 = new("db6f6ddb-ac77-4e88-8253-819df9bbf140");
    public static readonly Guid Dxgi = new("ca11c036-0102-4a2d-a6ad-f03cfed5d3c9");
    private readonly long[] _all = new long[3], _target = new long[3];

    public void Observe(Guid provider, int eventPid, int targetPid)
    {
        var index = provider == DxgKrnl ? 0 : provider == Direct3D11 ? 1 : provider == Dxgi ? 2 : -1;
        if (index < 0) return;
        _all[index]++;
        if (eventPid == targetPid) _target[index]++;
    }

    public Dictionary<string, object?> Report(int eventsLost)
    {
        var names = new[] { "Microsoft-Windows-DxgKrnl", "Microsoft-Windows-Direct3D11", "Microsoft-Windows-DXGI" };
        var absent = names.Where((_, i) => _all[i] == 0).ToArray();
        return new()
        {
            ["scope"] = "system-wide provider presence; target PID counts use event headers only",
            ["providerEvents"] = names.Select((name, i) => new Dictionary<string, object?>
                { ["provider"] = name, ["systemWide"] = _all[i], ["targetPid"] = _target[i] }).ToArray(),
            ["absentProviders"] = absent,
            ["eventsLost"] = eventsLost,
            ["status"] = eventsLost != 0 ? "events_lost" : absent.Length != 0 ? "providers_absent" : "providers_observed_no_reported_loss",
            ["gpuBusyTimeAnalyzed"] = false,
            ["qualification"] = "Event counts are coverage evidence, not GPU busy time or proof of a complete queue timeline. " +
                "GPU contention/preemption requires analysis of the retained ETL; kernel event PID is not context ownership."
        };
    }
}
