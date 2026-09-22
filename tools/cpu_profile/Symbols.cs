using Microsoft.Diagnostics.Symbols;

// Symbolization of EDVR's OWN modules, and only those: the graphics half
// (d3d11.dll in the game directory) and the native runtime (openvr_api.dll).
// The game has no PDB and never will, so nothing here touches it.
//
// A PDB is used only when its GUID and age match the CodeView record in the
// DLL the trace actually ran. A PDB from a rebuild of the same commit has a
// different GUID, so it is reported as unmatched and NOT used: symbolizing one
// build's RVAs with another build's PDB produces names that look right and are
// wrong, which is worse than no names at all.
//
// The symbol path is the given directory and nothing else. No symbol server, no
// network, no download.

internal sealed record SymbolHit(string Name, string File, int Line);

internal sealed class ModuleSymbolStatus
{
    public string Module = "";
    public string ModulePath = "";
    public string PdbName = "";
    public string PdbSignature = "";
    public int PdbAge;
    public bool Matched;
    public string PathUsed = "";
    public string FoundSignature = "";
    public int FoundAge;
    public string Note = "";

    public Dictionary<string, object?> ToJson() => new()
    {
        ["module"] = Module,
        ["modulePath"] = ModulePath,
        ["pdbName"] = PdbName,
        ["signature"] = PdbSignature,
        ["age"] = PdbAge,
        ["matched"] = Matched,
        ["pathUsed"] = PathUsed,
        ["foundSignature"] = FoundSignature,
        ["foundAge"] = FoundAge,
        ["note"] = Note,
    };
}

internal sealed class SymbolTable
{
    private readonly Func<int, uint, SymbolHit?>? _resolve;
    private readonly Dictionary<(int Module, uint Rva), SymbolHit?> _cache = [];
    public string Directory { get; }
    public List<ModuleSymbolStatus> Modules { get; } = [];
    public string Log = "";
    public long ResolvedLookups;
    public long UnresolvedLookups;

    private SymbolTable(Func<int, uint, SymbolHit?>? resolve, string directory)
    {
        _resolve = resolve;
        Directory = directory;
    }

    public bool Enabled => _resolve is not null;

    public static SymbolTable Off() => new(null, "");

    /// Off, but remembering where it looked: the report still has to say which
    /// directory was searched and what was found there.
    public static SymbolTable Disabled(string directory) => new(null, directory);

    /// The self-test's seam: a resolver with no PDB, no DIA and no trace behind
    /// it, so the additive report shape is testable on any machine.
    public static SymbolTable ForTest(Func<int, uint, SymbolHit?> resolve, string directory = "fake") =>
        new(resolve, directory);

    public SymbolHit? Resolve(int moduleId, uint rva)
    {
        if (_resolve is null || moduleId < 0) return null;
        if (_cache.TryGetValue((moduleId, rva), out var cached))
        {
            if (cached is null) UnresolvedLookups++; else ResolvedLookups++;
            return cached;
        }
        SymbolHit? hit = null;
        try { hit = _resolve(moduleId, rva); }
        catch { hit = null; }
        _cache[(moduleId, rva)] = hit;
        if (hit is null) UnresolvedLookups++; else ResolvedLookups++;
        return hit;
    }

    /// Adds name/file/line beside an RVA string. The RVA itself never changes,
    /// so a consumer that does not know about symbols reads the same report.
    public void Annotate(Dictionary<string, object?> row, int moduleId, string rvaText)
    {
        if (!Enabled) return;
        if (!TryParseRva(rvaText, out var rva)) return;
        var hit = Resolve(moduleId, rva);
        if (hit is null) return;
        row["name"] = hit.Name;
        row["file"] = hit.File;
        row["line"] = hit.Line;
    }

    public static bool TryParseRva(string text, out uint rva)
    {
        rva = 0;
        if (string.IsNullOrEmpty(text)) return false;
        var body = text.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? text[2..] : text;
        return uint.TryParse(body, System.Globalization.NumberStyles.HexNumber, null, out rva);
    }

    public Dictionary<string, object?> ToJson() => new()
    {
        ["enabled"] = Enabled,
        ["directory"] = Directory,
        ["modules"] = Modules.Select(module => module.ToJson()).ToArray(),
        ["resolvedEdvrFrameLookups"] = ResolvedLookups,
        ["unresolvedEdvrFrameLookups"] = UnresolvedLookups,
        ["distinctRvasLookedUp"] = _cache.Count,
        ["distinctRvasResolved"] = _cache.Count(pair => pair.Value is not null),
        ["log"] = Log.Length > 4000 ? Log[..4000] : Log,
    };
}

internal static class PdbSymbols
{
    /// Opens whatever in the directory matches EDVR's own modules. Anything that
    /// throws - a missing DIA, an unreadable PDB - degrades to "no symbols" with
    /// the reason recorded, never to a failed analysis.
    public static SymbolTable Load(string directory, IReadOnlyList<ModuleInfo> modules)
    {
        var wanted = modules.Where(module =>
            module.Class is ModuleClass.EdvrD3d11 or ModuleClass.EdvrOpenvrApi).ToArray();
        var log = new StringWriter();
        var opened = new Dictionary<int, NativeSymbolModule>();
        var statuses = new List<ModuleSymbolStatus>();
        SymbolReader? reader = null;
        try { reader = new SymbolReader(log, directory, null); }
        catch (Exception ex) { log.WriteLine($"SymbolReader unavailable: {ex.Message}"); }

        foreach (var module in wanted)
        {
            var status = new ModuleSymbolStatus
            {
                Module = module.Display,
                ModulePath = module.Path,
                PdbName = module.PdbName,
                PdbSignature = module.PdbSignature.ToString(),
                PdbAge = module.PdbAge,
            };
            statuses.Add(status);
            if (reader is null) { status.Note = "no symbol reader"; continue; }
            var fileName = module.PdbName.Length > 0 ? Path.GetFileName(module.PdbName) : "";
            if (fileName.Length == 0) { status.Note = "the module carries no CodeView PDB name"; continue; }
            try
            {
                var found = reader.FindSymbolFilePath(fileName, module.PdbSignature, module.PdbAge,
                                                      module.Path, "", false);
                if (found is not null && File.Exists(found))
                {
                    var symbolModule = reader.OpenNativeSymbolFile(found);
                    if (symbolModule.PdbGuid == module.PdbSignature && symbolModule.PdbAge == module.PdbAge)
                    {
                        status.Matched = true;
                        status.PathUsed = found;
                        status.FoundSignature = symbolModule.PdbGuid.ToString();
                        status.FoundAge = symbolModule.PdbAge;
                        opened[module.Id] = symbolModule;
                        continue;
                    }
                    status.Note = "the PDB found does not carry the DLL's signature";
                    status.FoundSignature = symbolModule.PdbGuid.ToString();
                    status.FoundAge = symbolModule.PdbAge;
                    continue;
                }
                // Nothing matched. Say what is actually in the directory under
                // that name, so an unmatched PDB is a diagnosis, not a silence.
                var candidate = Path.Combine(directory, fileName);
                if (!File.Exists(candidate))
                {
                    status.Note = $"no {fileName} in the symbol directory";
                    continue;
                }
                status.PathUsed = "";
                try
                {
                    var probe = reader.OpenNativeSymbolFile(candidate);
                    status.FoundSignature = probe.PdbGuid.ToString();
                    status.FoundAge = probe.PdbAge;
                    status.Note = "signature mismatch: this PDB is from a different build of the same source";
                }
                catch (Exception ex)
                {
                    status.Note = $"{fileName} is present but could not be read: {ex.Message}";
                }
            }
            catch (Exception ex)
            {
                status.Note = $"lookup failed: {ex.Message}";
            }
        }

        var table = opened.Count == 0
            ? SymbolTable.Disabled(directory)
            : SymbolTable.ForTest((moduleId, rva) => Lookup(opened, moduleId, rva), directory);
        foreach (var status in statuses) table.Modules.Add(status);
        table.Log = log.ToString();
        return table;
    }

    private static SymbolHit? Lookup(Dictionary<int, NativeSymbolModule> opened, int moduleId, uint rva)
    {
        if (!opened.TryGetValue(moduleId, out var symbolModule)) return null;
        var name = symbolModule.FindNameForRva(rva);
        if (string.IsNullOrEmpty(name)) return null;
        var file = "";
        var line = 0;
        try
        {
            var location = symbolModule.SourceLocationForRva(rva);
            if (location?.SourceFile is not null)
            {
                file = location.SourceFile.BuildTimeFilePath ?? "";
                line = location.LineNumber;
            }
        }
        catch { /* line info is optional; a name alone is still worth having */ }
        return new SymbolHit(name, file, line);
    }
}
