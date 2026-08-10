# Avenor Spine PCG setup

## What C++ now supplies

`ASpineGenerator` no longer constructs a cube motorway and parcel field with
instanced-mesh components. It owns the editable alignment and supplies PCG with:

- `GuideSpline`, tagged `Avenor.Spine.Center`;
- `HighwayPositiveSpline`, tagged `Avenor.Highway.Positive`;
- `HighwayNegativeSpline`, tagged `Avenor.Highway.Negative`;
- `MonorailPositiveSpline`, tagged `Avenor.Monorail.Positive`;
- `MonorailNegativeSpline`, tagged `Avenor.Monorail.Negative`;
- `StationRecords`;
- `BlockRecords`;
- `GreyboxSegments`;
- `GuidewayPlacements`;
- `MonorailPierPlacements`;
- `MonorailSupportPlacements`;
- `StreetLampPlacements`.

The station and block arrays contain stable IDs and world transforms. The
greybox array contains disposable box transforms for the complete highway,
twin guideways and local street grid. These arrays are flat reflected structs,
so PCG 5.8 can read them with stock `Get Actor Property` nodes.

Generated arrays are transient and rebuilding is deliberately button-driven.
Moving, selecting or editing the actor does not rebuild or serialize the
derived records into the level. The safe prototype defaults are one district,
one development row on each side and non-partitioned generation.

The actor also owns `SpineTerrainCorridor`, a Mesh Terrain development-grading
modifier. It does not create another terrain mesh. **Rebuild Terrain Alignment**
samples the existing Mesh Terrain every 25 m along the route and in 25 m bands
across the complete development footprint. It smooths those lateral profiles,
favours cutting over large embankments and enforces the configured road and
cross grades. The road reservation and each adjoining frontage start on the
shared datum; ground then follows the independently solved left or right
profile, so one side can climb while the other descends. The same solved road
profile drives roads, stations, guideways and piers. The solved ground profiles
are saved with the level; the larger disposable PCG placement arrays remain
transient.

The binding district arithmetic is:

| Element | Length |
|---|---:|
| Station-to-station chainage | 102,400 cm / 1,024 m |
| Nine clear block bays | 90,000 cm / 900 m |
| One station cross-street | 2,000 cm / 20 m |
| Eight resolved local streets | 8 × 1,300 cm / 13 m |
| Total | 102,400 cm / 1,024 m |

Roads are outside the guaranteed 100 × 100 m block dimensions. The configured
public-realm bay is tagged `PublicRealm`; the other eight bays are tagged
`Development`.

## Create the first functional graph

Create `/Game/Avenor/PCG/Spine/PCG_Spine_Master`.

### 1. Greybox infrastructure branch

1. Add **Get Actor Property**.
2. Set Actor Selection to **Self** and Property Name to
   `GreyboxSegments`. Self is the `SpineGenerator` that owns this PCG
   component.
3. Add **Attribute Set To Point** and map the struct's `Transform` attribute to
   the point property `$Transform`.

The generated transforms follow the same solved development surface used by
Mesh Terrain. Cross-streets rise or fall away from the Spine, and block surface
transforms use the local terrain normal instead of remaining level at the
central road datum.
4. Add a **Static Mesh Spawner**, add one Mesh Entry, and assign
   `/Engine/BasicShapes/Cube`. The Debug Point Mesh does not count as a mesh
   entry.
5. Preserve the incoming point transform and scale. Do not randomise rotation
   or scale: each point already represents an exact road or guideway span.
6. For clearer colours, split on the `Kind` attribute before the spawner:
   - `Highway`;
   - `LocalStreet`;
   - `StationStreet`.
7. Use one material per branch, then gather the outputs.

This branch is the replacement functional greybox. It is intentionally crude
in appearance, but its station cadence, independent guideways, clear block
dimensions and road reservations are real rather than illustrative.

### 2. Block branch

1. Add **Get Actor Property** for `BlockRecords`, selecting the same
   **Self** actor.
2. Convert the Attribute Set to points by mapping `Transform` to `$Transform`.
3. Split or filter on `ZoneRole` (`Development` or `PublicRealm`).
4. For the initial visual test, spawn a 100 × 100 × 0.2 m cube pad at each
   point. Set scale explicitly here because block transforms intentionally do
   not contain display scale.
5. Give public-realm pads a different material.

Do not turn these PCG points into live ownership records. PCG proposes the
layout; publishing a validated layout manifest to the world registry is a
separate future operation. After publication, the registry snapshot will be
the parcel authority and the graph will consume that versioned snapshot.

### 3. Station branch

1. Add **Get Actor Property** for `StationRecords`, selecting the same
   **Self** actor.
2. Convert the Attribute Set to points by mapping `Transform` to `$Transform`.
3. Initially use **Spawn Actor** to place a reusable
   `BP_Station_Standard_Greybox` made from the supplied station blockout.
4. Keep the station actor origin at road datum. The record's `PlatformDatum`
   is 930 cm and the v01 greybox already uses that datum.
5. Map `StationId` and `StationIndex` to variables on the station controller
   when that Blueprint exists.

The production station may later wrap a regular Level Instance, or use a
Packed Level Actor for a static shell plus a separate controller. PCG places
the station during editor generation; World Partition streams the baked actor.

### 4. Street-lamp branch

1. Add **Get Actor Property**.
2. Set Actor Selection to **Self** and Property Name to
   `StreetLampPlacements`.
3. Add **Attribute Set To Point** and map the struct's `Transform` attribute to
   the point property `$Transform`.
4. Add **Spawn Actor** and assign the reusable street-lamp Blueprint. Preserve
   the incoming point transform; do not randomise rotation, translation or
   scale.
5. Connect this branch to the same final gather/output as the other Spine
   infrastructure branches.

The C++ data already supplies every placement. Do not add a Spline Sampler,
Transform Points offset or density node to this branch. The default rules are:

- 5,000 cm / 50 m spacing (half one 100 m parcel);
- opposite road edges staggered by 2,500 cm / 25 m;
- pole centre 50 cm beyond the carriageway edge;
- both edges of both highway carriageways;
- both edges of every longitudinal local road and cross-street;
- 500 cm / 5 m clear of local junction approaches;
- local +X of the Blueprint faces the road.

If the Blueprint's lamp head points along +Y, set **Street Lamp Yaw Offset** on
`SpineGenerator` to `-90`. Use `180` for -X or `90` for -Y. These settings are
exposed under **Avenor | Street Lamps**, along with spacing, setback, junction
clearance and a generation toggle.

Using **Spawn Actor** is appropriate while validating the working Blueprint
and its real light. At production extent, replace the always-live Blueprint
lights with instanced lamp meshes plus distance-activated lights around
occupied areas; tens of thousands of independently active light actors are not
a viable VR target.

### 5. Output and generation settings

1. Gather the four branches and connect them to **Output**.
2. Keep the Spine PCG component non-partitioned for the prototype.
3. Do not enable runtime generation.
4. Assign `PCG_Spine_Master` to `Infrastructure Graph` on `SpineGenerator`.
5. Click **Regenerate Infrastructure**. It rebuilds the transient layout data
   before running the graph; a separate rebuild is unnecessary.

## Terrain alignment workflow

1. Assign the level's existing Mesh Partition actor to **Mesh Partition
   Actor** on `SpineGenerator`.
2. Keep the initial terrain values at:
   - sampling: 2,500 cm / 25 m;
   - smoothing: 25,000 cm / 250 m;
   - cut bias: 0.65;
   - maximum grade: 0.04 / 4%;
   - flat half-width: 2,700 cm / 27 m;
   - maximum development cross-grade: 0.06 / 6%;
   - lateral profile sampling: 2,500 cm / 25 m;
   - lateral profile smoothing: 10,000 cm / 100 m along the route;
   - outer blend distance: 12,000 cm / 120 m beyond the last parcel row.
3. Use **Regenerate Complete Spine** after moving the guide spline or changing
   terrain. This solves the terrain corridor first and regenerates PCG from
   the same profile.
4. Use **Regenerate Infrastructure** for quick mesh/material/PCG iteration
   when neither the route nor terrain has changed.

The Spine modifier uses the last Mesh Partition priority layer at priority 5.
Native Avenor water modifiers use priority 10, so rivers remain able to cut
through the corridor and identify future bridge crossings rather than being
silently blocked by an embankment.

Partitioned generation and spatial culling belong to the later production
graph, after its bounds and World Partition behaviour have been tested. Do not
enable them for this first one-district verification graph.

## Production replacement graph

The box transforms are only the first verification layer. Production subgraphs
should read the tagged derived splines:

- sample the highway splines for modular carriageway meshes, markings and
  barriers;
- sample both monorail splines independently for guideway meshes;
- sample support points, then difference them against station, road and water
  footprints;
- use `BlockRecords` for block surfaces and the authoritative registry snapshot
  for parcel contents;
- rebuild terrain alignment before regenerating PCG infrastructure; the terrain
  pass clears existing generated road/block collision so it cannot be sampled
  as natural ground;
- use `StationRecords` to place authored station variants;
- consume `StreetLampPlacements` in a dedicated lighting subgraph, and run
  separate subgraphs for pavements, crossings, signs, trees and other dressing.

Keep these as subgraphs under `PCG_Spine_Master`; do not build one unmaintainable
wall of nodes.

## Why the graph is not created in source code

PCG graph assets are editable Unreal `.uasset` files. Unreal editor scripting
and C++ can technically create and modify graph objects, but doing so depends
on editor-only, version-sensitive APIs and produces a binary asset anyway. It
would make a simple graph harder to inspect and maintain.

The durable division is therefore:

- C++ creates deterministic, testable spatial data and any genuinely custom
  PCG elements;
- the PCG editor owns graph topology and asset choices;
- the resulting `.uasset` is committed to source control.

Stock PCG 5.8 nodes are sufficient for this initial graph, so a custom PCG
element is unnecessary.
