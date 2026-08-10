# Avenor Mesh Terrain + PCG World Pipeline

## Ownership

- `AProceduralTerrainGenerator` is the deterministic world definition. Its
  legacy class name remains so existing level references continue to load.
- One Mesh Partition actor owns terrain geometry, spatial sections, modifier
  layers, weight channels and compiled platform representations.
- PCG reads and writes the Mesh Partition through
  `PCGMeshPartitionInterop`.
- Native Water Body actors own visible rivers and lakes.
  `MeshPartitionWater` connects them to River and Lake terrain modifiers.
- `ASpineGenerator::GuideSpline` is the authoritative Spine route. The actor
  resolves that route into a terrain-sampled, grade-limited vertical alignment
  and exact station, block and infrastructure data for PCG.
- `ASpineGenerator::SpineTerrainCorridor` grades the road reservation and the
  complete adjoining development footprint on that same Mesh Partition. It
  pins adjoining frontages to the road datum, then follows independently
  sampled, smoothed and slope-limited ground profiles on the two sides before
  blending back outside the developed edge without creating overlapping ground.
- PCG proposes the initial block geometry. A future versioned world-registry
  snapshot becomes authoritative once that layout is published.
- `AParcelGenerator` is legacy terrain-analysis/visualisation code and must
  not be treated as the permanent parcel authority.

The game module deliberately stores the experimental Mesh Partition actor as
an `AActor` reference. Mesh Terrain work happens through assets, modifiers and
PCG graphs, avoiding unnecessary dependency on experimental C++ class names
that may change in the next engine release.

## Required plugins

- `MeshPartition`
- `MeshPartitionWater`
- `PCG`
- `PCGGeometryScriptInterop`
- `PCGMeshPartitionInterop`
- `MeshTerrainMode`
- `Water`

These are enabled by `Avenor.uproject`.

## One-time level migration

1. Open or create an **Open World** level with World Partition enabled.
2. Delete all old Landscape actors and every obsolete actor labelled
   `Avenor Generated Landscape`, `Avenor Generated Water Zone`,
   `Avenor River ...` or `Avenor Lake ...`.
3. Keep one `ProceduralTerrainGenerator` and one `SpineGenerator`.
4. Enter Mesh Terrain Mode and create a rectangular Mesh Partition actor.
5. Create and assign `MPD_AvenorWorld`, the Mesh Partition Definition.
6. Assign the Mesh Partition actor to `Mesh Terrain Actor` on the world
   definition.
7. Define these modifier priority layers in `MPD_AvenorWorld`, in order:
   - `BaseForm`
   - `SpineCorridor`
   - `RegionalRelief`
   - `Hydrology`
   - `Development`
   - `LocalDetail`
   - `ManualOverride`
8. Define these weight channels:
   - `Grass`
   - `Forest`
   - `Rock`
   - `Wetland`
   - `RiverBank`
   - `Sand`
   - `Developable`
   - `Road`
   - `SpineExclusion`
   - `Water`
9. Add one Water Zone. Add the Mesh Partition integration component required
   by Mesh Terrain to each Water Body used by the Water graph.
10. Create `PCG_Spine_Master` using `SPINE_PCG_SETUP.md`, then assign it to
    `Infrastructure Graph` on `SpineGenerator`.

## Graph contract

All graphs use the fixed `WorldSeed`. Generation is an editor/build operation,
then Mesh Partition compiles spatial runtime sections. Clients never generate
independent random terrain.

### Terrain graph

1. Query the Mesh Partition up to the appropriate input priority layer.
2. Generate or transform Dynamic Mesh geometry inside the PCG volume.
3. Write back to the matching priority/sub-priority. Do not use an inclusive
   query that reads the graph's own output, because that creates a feedback
   loop.
4. Use the Spine spline and distance field to keep roughly 1 km on either side
   broadly rolling and developable.
5. Blend progressively into regional hills.
6. Add rare mountain regions only at large Spine distances.
7. Use spline remesh/tessellation around rivers, roads and developed parcels;
   keep distant empty regions coarse.

### Water graph

- Read deterministic `Watercourses` from the world-definition actor.
- Each entry contains a source lake, connected downhill river points, surface
  heights and width.
- Create/update Water Body Lake and Water Body River actors.
- Pair them with Lake and River Mesh Terrain modifiers on the `Hydrology`
  priority layer.
- Write `Water`, `Wetland`, `RiverBank` and `Sand` channels from the same
  geometry used to shape the banks.

### Vegetation graph

- Query the compiled terrain surface and weight channels.
- Exclude `SpineExclusion`, `Road`, `Water` and developed parcel masks.
- Select biomes using height, slope, moisture and the terrain channels.
- Use partitioned PCG, hierarchical instancing and aggressive VR culling.

### Spine infrastructure graph

- Read `GreyboxSegments`, `BlockRecords`, `StationRecords` and
  `StreetLampPlacements` from the
  `SpineGenerator` with `Get Actor Property`.
- Read the tagged derived highway and twin-guideway splines for production
  mesh generation. Never make opposite-direction trams share one guideway.
- Read the Spine-owned development-corridor modifier and `SpineExclusion`
  channel; PCG does not duplicate terrain grading.
- Generate highway, guideway meshes and supports along the derived splines.
- Exclude supports from water, roads and station footprints.
- Place reusable station actors/instances at `StationRecords` transforms.
- Generate blocks and local roads from the exact 1,024 m district records.
- Keep signage, lighting and street furniture in separately controllable
  subgraphs and density/LOD groups.

### Parcels and roads

- Analyse the final Mesh Partition surface, not an independent heightmap.
- Classify submerged and mixed-water parcels from the `Water` channel and
  Water Body geometry.
- Generate access roads only for near-Spine/developed rows.
- Grade roads through the `Development` modifier layer and remesh locally.

## Editor buttons

- `Regenerate` rebuilds deterministic world-rule data and runs every assigned
  PCG graph.
- Individual terrain, water, vegetation and infrastructure graphs can be
  regenerated independently.
- `Clear Generated Terrain` only clears PCG output and cached rules. It cannot
  delete the Mesh Partition actor or Water actors.
