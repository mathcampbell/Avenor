# Hydrology RVT save/reload regression check

The writer now owns a non-transient static mesh, built through the editor
path with its mesh description committed. The component's mesh reference
and the mesh source data must survive saving the writer's level/package.
RVT pages themselves remain a runtime cache, not a saved mask image.

## Migration and check (Unreal Editor required)

1. Close Unreal and build the Avenor editor target from the updated source.
2. Open the level. Confirm the generator's RVT and writer material assignments.
3. Regenerate the hydrology writer once (existing transient meshes cannot be
   recovered by merely opening an old save). Check the bank/bed masks appear.
4. Select `Avenor_Hydrology_RVT_Writer` and its `HydrologyRvtMesh` component.
   Confirm its Static Mesh is `HydrologyRvtStaticMesh` and its RVT is assigned.
5. Save All, including the level/external actor packages, and close normally.
6. Reopen without regenerating. Confirm the same mesh/material/RVT references
   are present and the masks reappear after RVT pages populate.
7. Repeat after regenerating and saving again: there should be one writer
   for this generator, with the newly generated masks, not an old duplicate.
8. Test a packaged client separately before shipping; this editor-side check
   does not constitute a cook/runtime validation.

Source checks alone cannot confirm Unreal serialization or rendered results.
