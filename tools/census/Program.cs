using System.Diagnostics;
using Census.Analysis;
using Census.Mo2;
using Census.Output;
using Census.Skyrim;

// Door census: every placed teleport door in a Skyrim SE load order, plus the spaces graph.
// Reads files only. See --help.

const string DefaultInstance = @"D:\AI\Active\Skyrim\instance";
const string DefaultOut = @"D:\AI\Active\SeamlessSkyrim\.weaver\scratch\census";

string instance = DefaultInstance, outDir = DefaultOut;
string? profile = null, dataDir = null;
float threshold = 1000f;

for (int i = 0; i < args.Length; i++)
{
    string Next() => i + 1 < args.Length ? args[++i] : throw new ArgumentException($"{args[i]} needs a value");
    switch (args[i])
    {
        case "--instance": instance = Next(); break;
        case "--profile": profile = Next(); break;
        case "--data": dataDir = Next(); break;
        case "--out": outDir = Next(); break;
        case "--shared-threshold": threshold = float.Parse(Next(), System.Globalization.CultureInfo.InvariantCulture); break;
        case "-h": case "--help":
            Console.WriteLine("""
                census - door census for a Skyrim SE load order managed by Mod Organizer 2 (read-only)

                  dotnet run -c Release --project tools/census -- [options]

                  --instance <dir>          MO2 portable instance        (default D:\AI\Active\Skyrim\instance)
                  --profile <name>          profile; default = selected_profile in ModOrganizer.ini
                  --data <dir>              game Data folder; default = gamePath from ModOrganizer.ini + \Data
                  --out <dir>               output folder                (default D:\AI\Active\SeamlessSkyrim\.weaver\scratch\census)
                  --shared-threshold <u>    max horizontal distance, in game units, between the two ends of a
                                            gate pair for it to count as coordinate-shared (default 1000)

                Writes census.json and summary.md to the output folder.
                """);
            return 0;
        default: Console.Error.WriteLine($"unknown argument {args[i]} (try --help)"); return 2;
    }
}

var sw = Stopwatch.StartNew();
void Log(string s) => Console.WriteLine($"[{sw.Elapsed.TotalSeconds,6:0.0}s] {s}");

var mo2 = Mo2Instance.Open(instance, profile, dataDir);
Log($"MO2 instance {mo2.InstanceDir}, profile {mo2.ProfileName}, {mo2.Mods.Count(m => m.Enabled)} enabled mods, data {mo2.GameDataDir}");

var data = SkyrimExtractor.Extract(mo2, Log);
foreach (var m in mo2.Mods.Where(m => m.Enabled && !m.DirExists))
    data.Issues.Add($"mod '{m.Name}' is enabled in modlist.txt but its folder is missing");

var graph = SpaceGraph.Build(data, threshold);
Log($"graph built: {graph.Doors.Total:N0} doors, {graph.Edges.Count:N0} space pairs");

var doc = new CensusDocument
{
    Meta =
    {
        Game = data.Game,
        Generated = DateTime.Now.ToString("yyyy-MM-dd HH:mm"),
        Instance = mo2.InstanceDir,
        Profile = mo2.ProfileName,
        GameData = mo2.GameDataDir,
    },
    Plugins = data.Plugins,
    Issues = data.Issues,
    Spaces = data.Spaces,
    Doors = data.Doors,
    Endpoints = data.Endpoints,
    Graph = graph,
};
ReportWriter.Write(outDir, doc);
Log($"wrote {Path.Combine(outDir, "census.json")} and summary.md");
return 0;
