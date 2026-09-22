using Microsoft.Diagnostics.Tracing.Etlx;

// Stacks are interned once per distinct TraceEvent CallStackIndex and held as
// (module id, RVA) pairs, never as formatted strings: a four-minute file-mode
// trace has millions of switch-outs and samples but only tens of thousands of
// distinct stacks, so classification is paid once per stack, not once per event.
//
// A frame whose code address TraceEvent could not attribute is not discarded.
// It is retried against the process's loaded-module ranges, which come from the
// Loader rundown for anything mapped before the session started, and whatever
// still fails is counted as unresolved rather than quietly becoming "other".

internal enum ModuleClass : byte
{
    Unknown = 0, GameExe, EdvrD3d11, EdvrOpenvrApi, SystemD3d11, NvidiaUserMode, GameModule, SystemModule,
    Kernel, Other
}

internal enum RvaKind : byte { Body, Thunk, CallSite, Eval, Lod, Census, Scheduler }

/// A single label per stack, by precedence, so one wait or one sample can be
/// attributed to one owner. Pipeline wins over the job scheduler itself, since a
/// worker inside the job pipeline is what the gate is asking about.
internal enum StackLabel : byte
{
    Empty = 0, Pipeline, Scheduler, GameOther, EdvrD3d11, EdvrOpenvrApi, OtherModule, KernelOnly
}

internal sealed record RvaClass(string Name, uint Entry, uint Bound, RvaKind Kind, bool Pipeline)
{
    public bool Contains(uint rva) => rva >= Entry && rva < Bound;
}

internal static class RvaTable
{
    private const uint BodyWindow = 0x2000;
    private const uint ThunkWindow = 0x10;
    private const uint CallSiteWindow = 0x10;

    // (name, entry, size, kind, pipeline). Size is the function's byte length
    // from the Ghidra decompile note analysis\decomp\decomp_<rva>.txt, which is
    // exact; zero means no decompile exists for that entry and the bound falls
    // back to the next known entry. reset_repopulate and census_bracket are
    // matched like job bodies but are deliberately outside "pipeline": one is a
    // one-shot settlement admission candidate, the other the draw-item builder,
    // and neither belongs to the steady-state chain the gate measures.
    private static readonly (string Name, uint Entry, uint Size, RvaKind Kind, bool Pipeline)[] Seeds =
    [
        ("update_render_data_job", 0x4321940, 503, RvaKind.Body, true),
        ("render_data_batch", 0x4320340, 565, RvaKind.Body, true),
        ("update_physics_objects_job", 0x432B2A0, 62, RvaKind.Body, true),
        ("table_ba0", 0x4321810, 0, RvaKind.Body, true),
        ("record_drain", 0x42DF940, 134, RvaKind.Body, true),
        ("reset_repopulate", 0x36A0F50, 687, RvaKind.Body, false),
        ("census_bracket", 0x42B4420, 0x1000, RvaKind.Census, false),
        ("thunk_42df520", 0x42DF520, 0, RvaKind.Thunk, true),
        ("thunk_42df540", 0x42DF540, 0, RvaKind.Thunk, true),
        ("thunk_42dfaf0", 0x42DFAF0, 0, RvaKind.Thunk, true),
        ("thunk_42dfba0", 0x42DFBA0, 0, RvaKind.Thunk, true),
        ("call_site_431b21f", 0x431B21F, 0, RvaKind.CallSite, true),
        ("eval_430efe0", 0x430EFE0, 1006, RvaKind.Eval, true),
        // Nine bytes: a jmp thunk, so this class will rarely match a sampled
        // frame. Kept because the name is the evaluator the arc is chasing.
        ("lod_evaluator_4331300", 0x4331300, 9, RvaKind.Lod, true),
    ];

    public const uint SchedulerStart = 0x5D6B20;
    public const uint SchedulerEnd = 0x5D7100;

    public static readonly RvaClass[] Classes = Build();
    public static readonly int CensusClassId = Array.FindIndex(Classes, c => c.Kind == RvaKind.Census);

    /// A function whose decompile note carries a size ends at entry+size. Where
    /// no decompile exists the bound is the next known entry (or entry+0x2000 if
    /// that is nearer), except census_bracket, whose 0x1000 is a stated guess. A
    /// thunk or a call site matches only the 16 bytes after it, which is where a
    /// return address lands.
    private static RvaClass[] Build()
    {
        var entries = Seeds.Select(seed => seed.Entry).Append(SchedulerStart).Distinct().OrderBy(x => x).ToArray();
        var classes = new List<RvaClass>();
        foreach (var seed in Seeds)
        {
            var next = entries.FirstOrDefault(entry => entry > seed.Entry, uint.MaxValue);
            var bound = seed.Kind switch
            {
                RvaKind.CallSite => seed.Entry + CallSiteWindow,
                RvaKind.Thunk => seed.Entry + ThunkWindow,
                _ when seed.Size > 0 => seed.Entry + seed.Size,
                _ => Math.Min(next, seed.Entry + BodyWindow),
            };
            classes.Add(new RvaClass(seed.Name, seed.Entry, bound, seed.Kind, seed.Pipeline));
        }
        classes.Add(new RvaClass("scheduler_range", SchedulerStart, SchedulerEnd, RvaKind.Scheduler, false));
        return classes.OrderBy(c => c.Entry).ToArray();
    }

    public static uint SizeOf(string name) => Seeds.FirstOrDefault(seed => seed.Name == name).Size;

    public static string BoundRule(RvaClass item) => item.Kind switch
    {
        RvaKind.CallSite => "entry+0x10 (return address after the call)",
        RvaKind.Thunk => "entry+0x10 (thunk)",
        RvaKind.Scheduler => "explicit range",
        RvaKind.Census => "entry+0x1000 (assumed, no decompile)",
        _ when SizeOf(item.Name) > 0 => $"decompile size ({SizeOf(item.Name)} bytes)",
        _ => item.Bound == item.Entry + BodyWindow ? "entry+0x2000" : "next known entry",
    };
}

internal sealed class ModuleInfo(int id, string path, string name, ulong imageBase, ModuleClass moduleClass)
{
    public int Id { get; } = id;
    public string Path { get; } = path;
    public string Name { get; } = name;
    public ulong ImageBase { get; } = imageBase;
    public ModuleClass Class { get; } = moduleClass;
    public long Frames;

    /// The two d3d11.dll are the same file name in different directories, and
    /// the report has to keep them apart wherever a module is named.
    public string Display => Class switch
    {
        ModuleClass.EdvrD3d11 => "d3d11.dll [edvr]",
        ModuleClass.SystemD3d11 => "d3d11.dll [system32]",
        _ => Name.Length > 0 ? Name.ToLowerInvariant() : "unknown",
    };
}

/// The topmost game-module RVAs of a stack, innermost first. Three is enough to
/// name a blocking site and its two callers, which is how the hand count of the
/// parked trace identified 0x5d6d7f under 0x5d4141 / 0x5d67a6.
internal readonly record struct GameTop(uint First, uint Second, uint Third, int Count);

internal sealed class StackInfo
{
    public int[] ModuleIds = [];
    public ulong[] Addresses = [];
    public int[] ClassIds = [];
    public StackLabel Label;
    public GameTop GameTops;
    public bool Pipeline;
    public bool Scheduler;
    public bool Census;
    public bool AnyEdvrD3d11;
    public bool AnySystemD3d11;
    public bool AnyNvidiaUserMode;
    public bool AnyUnresolved;
    public bool TopUnresolved;
    public int TopModuleId = -1;
    public int Depth;
    public bool Truncated;
}

internal sealed class StackStore
{
    public const int MaxDepth = 192;

    private readonly TraceLog? _log;
    private readonly TraceLoadedModules? _processModules;
    private readonly TraceLoadedModules? _kernelModules;
    private readonly Dictionary<CallStackIndex, int> _ids = [];
    private readonly List<StackInfo> _stacks = [];
    private readonly Dictionary<ModuleFileIndex, int> _moduleIds = [];
    private readonly List<ModuleInfo> _modules = [];
    private readonly List<int> _classScratch = [];
    public string GameDirectory { get; }
    public long ResolvedFrames;
    public long RecoveredFrames;
    public long UnresolvedFrames;

    public StackStore(TraceLog log, int pid)
    {
        _log = log;
        // The game directory decides which d3d11.dll is EDVR's, so it is resolved
        // from the loaded module list before any stack is classified.
        var exe = log.ModuleFiles.FirstOrDefault(module =>
            string.Equals(System.IO.Path.GetFileName(module.FilePath), "elitedangerous64.exe",
                          StringComparison.OrdinalIgnoreCase));
        GameDirectory = exe is null ? "" : (System.IO.Path.GetDirectoryName(exe.FilePath) ?? "");
        // Anything mapped before the session started is only in the Loader
        // rundown, so a code address TraceEvent leaves unattributed is retried
        // against these ranges before it is called unresolved.
        _processModules = log.Processes.LastProcessWithID(pid)?.LoadedModules;
        _kernelModules = log.Processes.LastProcessWithID(0)?.LoadedModules;
    }

    /// Self-test mode: stacks are supplied directly instead of resolved from a
    /// trace, so every computation over classified stacks is testable offline.
    public StackStore(string gameDirectory = "")
    {
        _log = null;
        GameDirectory = gameDirectory;
    }

    public int Count => _stacks.Count;
    public IReadOnlyList<ModuleInfo> Modules => _modules;
    public StackInfo this[int id] => _stacks[id];

    public int AddForTest(StackInfo info)
    {
        _stacks.Add(info);
        return _stacks.Count - 1;
    }

    public int AddModuleForTest(string path, string name, ulong imageBase)
    {
        _modules.Add(new ModuleInfo(_modules.Count, path, name, imageBase, Classify(path, name)));
        return _modules.Count - 1;
    }

    public int Intern(CallStackIndex index, double timeMSec)
    {
        if (_log is null || index == CallStackIndex.Invalid) return -1;
        if (_ids.TryGetValue(index, out var existing)) return existing;
        var info = Classify(index, timeMSec);
        var id = _stacks.Count;
        _stacks.Add(info);
        _ids[index] = id;
        return id;
    }

    private StackInfo Classify(CallStackIndex index, double timeMSec)
    {
        var moduleIds = new List<int>();
        var addresses = new List<ulong>();
        _classScratch.Clear();
        var info = new StackInfo();
        uint top1 = 0, top2 = 0, top3 = 0;
        var gameCount = 0;
        for (var frame = index; frame != CallStackIndex.Invalid; frame = _log!.CallStacks.Caller(frame))
        {
            if (moduleIds.Count >= MaxDepth) { info.Truncated = true; break; }
            var codeIndex = _log!.CallStacks.CodeAddressIndex(frame);
            if (codeIndex == CodeAddressIndex.Invalid) continue;
            var address = _log.CodeAddresses.Address(codeIndex);
            var moduleIndex = _log.CodeAddresses.ModuleFileIndex(codeIndex);
            if (moduleIndex == ModuleFileIndex.Invalid)
            {
                var recovered = Recover(address, timeMSec);
                if (recovered == ModuleFileIndex.Invalid) UnresolvedFrames++;
                else { moduleIndex = recovered; RecoveredFrames++; }
            }
            else ResolvedFrames++;
            var moduleId = ModuleIdFor(moduleIndex);
            moduleIds.Add(moduleId);
            addresses.Add(address);
            if (moduleId < 0)
            {
                info.AnyUnresolved = true;
                continue;
            }
            var module = _modules[moduleId];
            module.Frames++;
            switch (module.Class)
            {
                case ModuleClass.EdvrD3d11: info.AnyEdvrD3d11 = true; break;
                case ModuleClass.SystemD3d11: info.AnySystemD3d11 = true; break;
                case ModuleClass.NvidiaUserMode: info.AnyNvidiaUserMode = true; break;
            }
            if (module.Class != ModuleClass.GameExe || address < module.ImageBase) continue;
            var rva = (uint)(address - module.ImageBase);
            gameCount++;
            if (gameCount == 1) top1 = rva;
            else if (gameCount == 2) top2 = rva;
            else if (gameCount == 3) top3 = rva;
            for (var c = 0; c < RvaTable.Classes.Length; c++)
                if (RvaTable.Classes[c].Contains(rva) && !_classScratch.Contains(c))
                    _classScratch.Add(c);
        }
        info.ModuleIds = moduleIds.ToArray();
        info.Addresses = addresses.ToArray();
        info.Depth = moduleIds.Count;
        info.ClassIds = _classScratch.ToArray();
        info.GameTops = new GameTop(top1, top2, top3, gameCount);
        info.TopModuleId = info.Depth > 0 ? info.ModuleIds[0] : -1;
        info.TopUnresolved = info.Depth > 0 && info.ModuleIds[0] < 0;
        foreach (var classId in info.ClassIds)
        {
            if (RvaTable.Classes[classId].Pipeline) info.Pipeline = true;
            if (RvaTable.Classes[classId].Kind == RvaKind.Scheduler) info.Scheduler = true;
            if (RvaTable.Classes[classId].Kind == RvaKind.Census) info.Census = true;
        }
        info.Label = Label(info, gameCount);
        return info;
    }

    private ModuleFileIndex Recover(ulong address, double timeMSec)
    {
        var module = _processModules?.GetModuleContainingAddress(address, timeMSec)
                     ?? _kernelModules?.GetModuleContainingAddress(address, timeMSec);
        return module?.ModuleFile?.ModuleFileIndex ?? ModuleFileIndex.Invalid;
    }

    private StackLabel Label(StackInfo info, int gameFrames)
    {
        var sawEdvrD3d11 = false;
        var sawEdvrOpenvr = false;
        var sawOther = false;
        foreach (var moduleId in info.ModuleIds)
        {
            if (moduleId < 0) continue;
            switch (_modules[moduleId].Class)
            {
                case ModuleClass.EdvrD3d11: sawEdvrD3d11 = true; break;
                case ModuleClass.EdvrOpenvrApi: sawEdvrOpenvr = true; break;
                case ModuleClass.Kernel: break;
                case ModuleClass.GameExe: break;
                default: sawOther = true; break;
            }
        }
        return LabelFor(info.Depth, info.Pipeline, info.Scheduler, gameFrames,
                        sawEdvrD3d11, sawEdvrOpenvr, sawOther);
    }

    /// One label per stack, by precedence. Pipeline wins over the job scheduler
    /// itself, and a game frame wins over the runtime and system modules below it.
    public static StackLabel LabelFor(int depth, bool pipeline, bool scheduler, int gameFrames,
                                      bool edvrD3d11, bool edvrOpenvrApi, bool otherModule)
    {
        if (depth == 0) return StackLabel.Empty;
        if (pipeline) return StackLabel.Pipeline;
        if (scheduler) return StackLabel.Scheduler;
        if (gameFrames > 0) return StackLabel.GameOther;
        if (edvrD3d11) return StackLabel.EdvrD3d11;
        if (edvrOpenvrApi) return StackLabel.EdvrOpenvrApi;
        if (otherModule) return StackLabel.OtherModule;
        return StackLabel.KernelOnly;
    }

    private int ModuleIdFor(ModuleFileIndex index)
    {
        if (index == ModuleFileIndex.Invalid) return -1;
        if (_moduleIds.TryGetValue(index, out var existing)) return existing;
        var module = _log!.ModuleFiles[index];
        var id = _modules.Count;
        var path = module.FilePath ?? "";
        var name = string.IsNullOrEmpty(path) ? module.Name ?? "" : System.IO.Path.GetFileName(path);
        _modules.Add(new ModuleInfo(id, path, name, module.ImageBase, Classify(path, name)));
        _moduleIds[index] = id;
        return id;
    }

    private ModuleClass Classify(string path, string name)
    {
        var lowerName = name.ToLowerInvariant();
        var lowerPath = path.ToLowerInvariant();
        if (lowerName.StartsWith("nvwgf2um", StringComparison.Ordinal) ||
            lowerName.StartsWith("nvd3dum", StringComparison.Ordinal) ||
            lowerName.StartsWith("nvldum", StringComparison.Ordinal))
            return ModuleClass.NvidiaUserMode;
        if (lowerName is "ntoskrnl.exe" or "ntkrnlmp.exe" or "ntdll.dll" or "hal.dll" or "halmacpi.dll" or
            "win32kbase.sys" or "win32kfull.sys" || lowerPath.Contains(@"\drivers\") ||
            lowerPath.EndsWith(".sys", StringComparison.Ordinal))
            return ModuleClass.Kernel;
        if (lowerName == "elitedangerous64.exe") return ModuleClass.GameExe;
        var inGameDirectory = GameDirectory.Length > 0 &&
            lowerPath.StartsWith(GameDirectory.ToLowerInvariant() + @"\", StringComparison.Ordinal);
        if (lowerName == "d3d11.dll")
            return inGameDirectory ? ModuleClass.EdvrD3d11
                 : lowerPath.Contains(@"\system32\") || lowerPath.Contains(@"\syswow64\")
                   ? ModuleClass.SystemD3d11 : ModuleClass.Other;
        if (lowerName == "openvr_api.dll")
            return inGameDirectory || lowerPath.Contains(@"\openvr\win64\")
                ? ModuleClass.EdvrOpenvrApi : ModuleClass.Other;
        if (inGameDirectory) return ModuleClass.GameModule;
        if (lowerPath.Contains(@"\windows\")) return ModuleClass.SystemModule;
        return ModuleClass.Other;
    }

    public string DisplayName(int moduleId) =>
        moduleId >= 0 && moduleId < _modules.Count ? _modules[moduleId].Display : "unresolved";

    public ModuleClass ClassOf(int moduleId) =>
        moduleId >= 0 && moduleId < _modules.Count ? _modules[moduleId].Class : ModuleClass.Unknown;

    /// The stack's frames inside EDVR's own d3d11.dll, innermost outward, as
    /// RVAs of that module, plus the first game-exe RVA below the last of them:
    /// the call site where the game entered our hook. Both are RVAs so another
    /// pass can symbolize them against the built DLL and the game image.
    public List<string> EdvrFrames(StackInfo info, int max, out string gameCallSite)
    {
        var chain = new List<string>();
        gameCallSite = "none";
        var lastEdvr = -1;
        for (var i = 0; i < info.ModuleIds.Length; i++)
        {
            var moduleId = info.ModuleIds[i];
            if (moduleId < 0 || _modules[moduleId].Class != ModuleClass.EdvrD3d11) continue;
            lastEdvr = i;
            if (chain.Count < max && info.Addresses[i] >= _modules[moduleId].ImageBase)
                chain.Add($"0x{info.Addresses[i] - _modules[moduleId].ImageBase:x}");
        }
        if (lastEdvr < 0) return chain;
        for (var i = lastEdvr + 1; i < info.ModuleIds.Length; i++)
        {
            var moduleId = info.ModuleIds[i];
            if (moduleId < 0 || _modules[moduleId].Class != ModuleClass.GameExe) continue;
            if (info.Addresses[i] >= _modules[moduleId].ImageBase)
                gameCallSite = $"0x{info.Addresses[i] - _modules[moduleId].ImageBase:x}";
            break;
        }
        return chain;
    }

    /// One stack as a single string: a game frame is its RVA, anything else is
    /// its module name, and a run of frames in the same module collapses to
    /// name*count so the line stays readable.
    public string Signature(StackInfo info, int maxTokens = 20)
    {
        var tokens = new List<string>();
        var run = "";
        var runCount = 0;
        for (var i = 0; i < info.ModuleIds.Length && tokens.Count < maxTokens; i++)
        {
            var moduleId = info.ModuleIds[i];
            var token = moduleId >= 0 && _modules[moduleId].Class == ModuleClass.GameExe &&
                        info.Addresses[i] >= _modules[moduleId].ImageBase
                ? $"0x{info.Addresses[i] - _modules[moduleId].ImageBase:x}"
                : DisplayName(moduleId);
            if (token == run) { runCount++; continue; }
            if (runCount > 0) tokens.Add(runCount > 1 ? $"{run}*{runCount}" : run);
            run = token;
            runCount = 1;
        }
        if (runCount > 0 && tokens.Count < maxTokens) tokens.Add(runCount > 1 ? $"{run}*{runCount}" : run);
        if (info.ModuleIds.Length > 0 && tokens.Count >= maxTokens) tokens.Add("...");
        return string.Join(" < ", tokens);
    }

    /// The legacy report format for a stack frame, preserved so the existing
    /// topStacksInPostPresent keys keep the shape earlier sessions read.
    public string[] Format(StackInfo info)
    {
        var frames = new string[info.ModuleIds.Length];
        for (var i = 0; i < info.ModuleIds.Length; i++)
        {
            var moduleId = info.ModuleIds[i];
            var address = info.Addresses[i];
            if (moduleId >= 0 && address >= _modules[moduleId].ImageBase)
                frames[i] = FormatModuleAddress(_modules[moduleId].Path, _modules[moduleId].ImageBase, address);
            else if (moduleId >= 0 && !string.IsNullOrWhiteSpace(_modules[moduleId].Name))
                frames[i] = $"{_modules[moduleId].Name}@0x{address:x}";
            else
                frames[i] = $"0x{address:x}";
        }
        return frames;
    }

    public static string FormatModuleAddress(string path, ulong imageBase, ulong address) =>
        $"{path}+0x{address - imageBase:x} [va=0x{address:x}]";
}
