using System.Text.Json.Serialization;

namespace Census.Model;

// Engine-agnostic census records. Only ids, strings, numbers and arrays: a Fallout 4 or Starfield
// extractor can fill the same shapes. Ids are "<plugin>:<localHexId>" strings (FormKey style),
// space ids are "interior:<id>" / "worldspace:<id>". Positions and rotations are [x, y, z] arrays
// in game units / radians.

public static class SpaceKind
{
    public const string Interior = "interior";
    public const string Worldspace = "worldspace";
}

public static class LinkStatus
{
    public const string Ok = "ok";                      // destination door points back at this door
    public const string OneWay = "one-way";             // destination is a teleport door that points elsewhere
    public const string DestNull = "dest-null";         // teleport data without a destination reference
    public const string DestMissing = "dest-missing";   // destination reference does not exist in the load order
    public const string DestNotTeleport = "dest-not-teleport"; // destination exists but has no teleport data
}

public static class DoorKind
{
    public const string InteriorExterior = "interior-exterior";
    public const string InteriorInterior = "interior-interior";
    public const string ExteriorExterior = "exterior-exterior"; // "gate"
    public const string Unknown = "unknown";
}

/// <summary>Where something stands: an interior cell, or a worldspace (plus exterior cell grid).</summary>
public sealed class Place
{
    public string SpaceId { get; set; } = "";
    public string Kind { get; set; } = SpaceKind.Interior;
    /// <summary>Cell the reference sits in (interior cell, or exterior/persistent cell).</summary>
    public string? CellId { get; set; }
    /// <summary>Exterior cell grid [x, y]; from the cell record, or from the position for persistent refs.</summary>
    public int[]? Grid { get; set; }
    public string? GridSource { get; set; }
}

public sealed class BaseObject
{
    public string? Id { get; set; }
    public string? Type { get; set; }
    public string? EditorId { get; set; }
    /// <summary>Mesh path as given in the record (relative to Data\Meshes).</summary>
    public string? Model { get; set; }
    public string[] Scripts { get; set; } = Array.Empty<string>();
    /// <summary>Door flags by name (Automatic, Hidden, MinimalUse, Sliding, DoNotOpenInCombatSearch).</summary>
    public string[] Flags { get; set; } = Array.Empty<string>();
    public bool MinimalUse { get; set; }
    public bool Automatic { get; set; }
}

public sealed class Destination
{
    /// <summary>Destination door reference id (null when the teleport data has none).</summary>
    public string? DoorId { get; set; }
    /// <summary>Where the player is placed (XTEL position / rotation).</summary>
    public float[] LandPos { get; set; } = new float[3];
    public float[] LandRot { get; set; } = new float[3];
    public string[] Flags { get; set; } = Array.Empty<string>();

    // Filled from the destination reference when it exists.
    public Place? At { get; set; }
    public float[]? DoorPos { get; set; }
    public float[]? DoorRot { get; set; }
    public string? DoorPlugin { get; set; }
}

public sealed class LockInfo
{
    public int Level { get; set; }
    public string LevelName { get; set; } = "";
    public string? KeyId { get; set; }
    public string[] Flags { get; set; } = Array.Empty<string>();
}

public sealed class DoorRecord
{
    public string Id { get; set; } = "";
    /// <summary>Plugin that last edits the reference (the winning override).</summary>
    public string Plugin { get; set; } = "";
    /// <summary>MO2 mod folder (or "game Data" / "overwrite") that supplies <see cref="Plugin"/>.</summary>
    public string? PluginSource { get; set; }
    /// <summary>Plugin that created the reference.</summary>
    public string Origin { get; set; } = "";
    /// <summary>base | dlc | creation | mod</summary>
    public string OriginKind { get; set; } = "mod";
    public BaseObject Base { get; set; } = new();
    public Place At { get; set; } = new();
    public float[] Pos { get; set; } = new float[3];
    public float[] Rot { get; set; } = new float[3];
    public Destination Dest { get; set; } = new();
    /// <summary>Null when the reference is not locked.</summary>
    public LockInfo? Lock { get; set; }
    public string[] Scripts { get; set; } = Array.Empty<string>();
    public bool HasScripts { get; set; }
    public bool BaseHasScripts { get; set; }
    public bool MinimalUse { get; set; }
    public bool Automatic { get; set; }
    public bool InitiallyDisabled { get; set; }
    public bool OpenByDefault { get; set; }

    // Filled by the analysis step.
    public string Kind { get; set; } = DoorKind.Unknown;
    public string Link { get; set; } = LinkStatus.Ok;
}

/// <summary>A teleport destination reference that is not itself a teleport door (or not found).</summary>
public sealed class Endpoint
{
    public string Id { get; set; } = "";
    public bool Exists { get; set; }
    public string? Plugin { get; set; }
    public string? BaseType { get; set; }
    public Place? At { get; set; }
    public float[]? Pos { get; set; }
    public float[]? Rot { get; set; }
}

public sealed class SpaceRecord
{
    public string Id { get; set; } = "";
    public string Kind { get; set; } = SpaceKind.Interior;
    public string? EditorId { get; set; }
    public string? Name { get; set; }
    public string Plugin { get; set; } = "";
    public string Origin { get; set; } = "";
    public string OriginKind { get; set; } = "mod";
    public string[] Flags { get; set; } = Array.Empty<string>();
    /// <summary>Worldspaces only: parent worldspace id and the "use parent data" flags.</summary>
    public string? ParentId { get; set; }
    public string[]? ParentFlags { get; set; }
    public int Doors { get; set; }
}

public sealed class PluginRecord
{
    public int Index { get; set; }
    public string Name { get; set; } = "";
    public bool Enabled { get; set; }
    public string? Path { get; set; }
    public string? Source { get; set; }
    public string? SourceKind { get; set; }
    public string[] AlsoIn { get; set; } = Array.Empty<string>();
    public string OriginKind { get; set; } = "mod";
    public bool Loaded { get; set; }
    public string[] Masters { get; set; } = Array.Empty<string>();
}

/// <summary>Everything an engine-specific extractor hands to the analysis.</summary>
public sealed class CensusData
{
    public string Game { get; set; } = "";
    public List<PluginRecord> Plugins { get; set; } = new();
    public List<DoorRecord> Doors { get; set; } = new();
    public List<Endpoint> Endpoints { get; set; } = new();
    public List<SpaceRecord> Spaces { get; set; } = new();
    public List<string> Issues { get; set; } = new();
    /// <summary>Per plugin: refs with teleport data created / overridden in that plugin (any override, winning or not).</summary>
    public Dictionary<string, PluginDoorTally> PluginTallies { get; set; } = new();
    public int ExteriorCellCount { get; set; }
}

public sealed class PluginDoorTally
{
    public int Added { get; set; }
    public int Edited { get; set; }
}
