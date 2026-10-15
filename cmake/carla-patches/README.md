# LibCarla patches

Fixes to upstream CARLA's LibCarla that typesafe_carla needs before upstream
has them. `cmake/FetchCarla.cmake` applies every `*.patch` here, in file-name
order, with `git apply` to the CARLA sources it fetches (never to a
`TSC_CARLA_SOURCE_DIR` tree, which is the user's). Their identity, the
*patch id* (`tools/carla_patches.py`, `tsc_carla_patch_id` in
`cmake/CarlaPatches.cmake`), is recorded in `BUILD_INFO.json`, in a LibCarla
prebuilt's `prebuilt.json` and in its artifact name, so a build never mixes
patched and unpatched LibCarla.

Rules:

- A patch is a `git diff` (paths `LibCarla/...` from the CARLA root),
  optionally preceded by a description, which `git apply` ignores.
- It must apply to every ref in `tools/release_carla_refs.txt` (and to the
  other refs CI builds, e.g. `0.10.0`), or be in it already (a ref that took
  the fix upstream: the patch applies in reverse and is skipped); CI's
  `libcarla` job fails to configure otherwise. `TSC_CARLA_PATCH_CHECK_DIRS=<carla checkout>[:...] uv run pytest
  tests/test_carla_patches.py` checks local checkouts.
- Each is proposed upstream, and is removed once every ref we build has the
  fix.

| Patch | Why | Upstream |
|---|---|---|
| `0001-trafficmanager-free-inmemorymap.patch` | The Traffic Manager's `InMemoryMap` leaks its whole waypoint graph (shared_ptr cycles between `SimpleWaypoint`s; ~400 MB on a large map) on every TM shut down and map change. | in ue58-dev (carla-simulator/carla#9975); still needed by ue5-dev and 0.10.0 |
| `0002-trafficmanager-clear-junction-entrances.patch` | The Traffic Manager's `LocalizationStage` never forgets a vehicle's junction entrance (`vehicles_at_junction_entrance`) on `RemoveActor`/`Reset`: it keeps removed vehicles' waypoints (of a destroyed `InMemoryMap`) alive and hands them to a vehicle that reuses the actor id. | proposed for ue58-dev (carla-simulator/carla#9977, draft); ue5-dev is no longer developed upstream |
