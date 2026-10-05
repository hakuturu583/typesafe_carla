# typesafe_carla

A statically typed CARLA client for [Codon](https://github.com/exaloop/codon).
Programs written against it are type-checked by the Codon compiler before they
run, and they call LibCarla through a narrow C ABI. CPython and the `carla`
Python package are not involved.

```python
import typesafe_carla as carla

client = carla.Client("localhost", 2000)
world = client.get_world()
vehicle = world.get_actors()[0].as_vehicle()
vehicle.apply_control(carla.VehicleControl(throttle=0.2, steer=0.0))

vehicle.apply_control(carla.Transform())
# error: 'Transform' does not match expected type 'VehicleControl'   <- at compile time
```

See [docs/design.md](docs/design.md) for the full design.

## Status

Milestones (design section 43):

| Milestone | Status |
|---|---|
| 0: proof of concept | ✅ verified against a CARLA 0.10.0 server |
| 1: usable vehicle API | ✅ verified against a CARLA 0.10.0 server |
| 2: sensors | ✅ verified against a CARLA 0.10.0 server |
| 3: distribution | ✅ release pipeline verified end to end (manylinux wheels from CI, clean-container `uv sync` → `build` → `./main` against a CARLA server); publishing to PyPI needs the one-time setup in [docs/releasing.md](docs/releasing.md) |
| 4: broader compatibility | ✅ verified against a CARLA 0.10.0 server |
| 5: binding generation | ✅ 252 C ABI functions generated from `bindings/*.yaml`, spec validated against LibCarla 0.10.0 and ue5-dev with libclang, [coverage report](docs/coverage.md) |

| Area | Implemented |
|---|---|
| Client | `Client` (`host="127.0.0.1"`, `port=2000`, `worker_threads=0`), `set_timeout`, `get_timeout`, `get_world`, `load_world` (`map_layers`: `MapLayer` flags), `reload_world`, `get_server_version`, `get_client_version`, `apply_batch`, `apply_batch_sync`, `get_trafficmanager`, `generate_opendrive_world` (+ `OpendriveGenerationParameters`), `get_available_maps`, `load_world_if_different` (`map_layers`; → `Optional[World]`), file transfer: `get_required_files`, `request_file`, `set_files_base_folder`; recorder: `start_recorder`, `stop_recorder`, `show_recorder_file_info`, `show_recorder_collisions`, `show_recorder_actors_blocked`, `replay_file`, `stop_replayer`, `set_replayer_time_factor`, `set_replayer_ignore_hero`, `set_replayer_ignore_spectator` |
| World | `id`, `get_actors` (also `get_actors(actor_ids: List[int])`: unknown ids are left out), `get_actor` (→ `Optional[Actor]`), `get_blueprint_library`, `spawn_actor`, `try_spawn_actor` (→ `Optional[Actor]`; both with `attach_to`, `attachment_type`), `tick`, `wait_for_tick`, `get_snapshot`, `get_map`, `get_settings`, `apply_settings`, `get_weather` / `set_weather` / `is_weather_enabled`, `get_random_location_from_navigation` (→ `Optional`), `debug` (`DebugHelper`: `draw_point`, `draw_line`, `draw_arrow`, `draw_box`, `draw_string`, `clear_debug_shape` / `clear_debug_string` (LibCarla ue5-dev)), `get_spectator`, traffic lights and signs: `get_traffic_light_from_opendrive_id` (→ `Optional[TrafficLight]`), `get_traffic_sign(landmark)` (→ `Optional[TrafficSign]`), `get_traffic_light(landmark)` (→ `Optional[TrafficLight]`), `get_traffic_lights_from_waypoint`, `get_traffic_lights_in_junction`, `freeze_all_traffic_lights`, `reset_all_traffic_lights`; `get_vehicles_light_states` (→ `Dict[int, int]`); environment: `get_environment_objects` (`EnvironmentObject`), `enable_environment_objects`, `get_level_bbs`, `get_names_of_all_objects` (`CityObjectLabel`); queries: `cast_ray`, `project_point` / `ground_projection` (→ `Optional[LabelledPoint]`); `load_map_layer` / `unload_map_layer` (`MapLayer`); `set_pedestrians_seed`, `set_pedestrians_cross_factor`, `get_imu_sensor_gravity` / `set_imu_sensor_gravity` (LibCarla ue5-dev); `on_tick` / `remove_on_tick` (dispatched like sensor callbacks); textures: `apply_color_texture_to_object(s)`, `apply_float_color_texture_to_object(s)`, `apply_textures_to_object(s)` |
| Actor | `id`, `type_id`, `is_alive`, `bounding_box`, `get/set_transform`, `get/set_location`, `get_velocity`, `set_target_velocity`, `get_acceleration`, `get_angular_velocity`, `destroy`, `set_target_angular_velocity`, `add_impulse`, `add_force`, `add_angular_impulse`, `add_torque`, `set_simulate_physics`, `set_enable_gravity`, `get_world` (issue #33); issue #19: `actor_state` (`ActorState`), `is_active`, `is_dormant`, `attributes` (`Dict[str, str]`), `parent` (→ `Optional[Actor]`), `semantic_tags` (`List[int]` of `CityObjectLabel`), `get_actor_name`, `get_actor_class_name`, `set_collisions`, `enable_constant_velocity` / `disable_constant_velocity`, `add_force_at_location`, `add_impulse_at_location`, `apply_texture` (`TextureColor` / `TextureFloatColor`, `MaterialParameter`), skeleton queries `get_bone_names`, `get_bone_world_transforms`, `get_bone_relative_transforms`, `get_component_names`, `get_component_world_transform`, `get_component_relative_transform`, `get_socket_names`, `get_socket_world_transforms`, `get_socket_relative_transforms` (LibCarla newer than 0.10.0 only, see below); checked `as_vehicle` / `as_sensor` / `as_walker` / `as_walker_ai_controller` / `as_traffic_light` / `as_traffic_sign`; the methods of Vehicle, Walker, WalkerAIController, TrafficLight, TrafficSign (`trigger_volume`, for signs and lights) and Sensor, as in the Python API (on a plain `Actor` only; checked at run time, `ActorTypeError` on the wrong kind; compile errors with `--strict`) |
| Vehicle | `apply_control`, `get_control`, `set_autopilot`, `get_physics_control` / `apply_physics_control` (every LibCarla UE5 field), `set_light_state` / `get_light_state` (`VehicleLightState`), `get_speed_limit`, `get_traffic_light_state`, `is_at_traffic_light`, `get_traffic_light` (→ `Optional`); issue #20: `apply_ackermann_control` (`VehicleAckermannControl`), `get_ackermann_controller_settings` / `apply_ackermann_controller_settings` (`AckermannControllerSettings`), `open_door` / `close_door` (`VehicleDoor`), `get_failure_state` (`VehicleFailureState`), `get_telemetry_data` (`VehicleTelemetryData`, `WheelTelemetryData`; LibCarla ue5-dev only), `show_debug_telemetry`, `get_wheel_steer_angle` / `set_wheel_steer_direction` (`VehicleWheelLocation`), `get_vehicle_bone_world_transforms` (LibCarla ue5-dev only), `enable_carsim`, `use_carsim_road`, `enable_chrono_physics` (server plugins) |
| Walkers | `Walker` (`apply_control(WalkerControl)`, `get_control`; issue #20: `get_bones` (→ `WalkerBoneControlOut` of `bone_transform_out`), `set_bones` (`WalkerBoneControlIn` of `bone_transform` or `(name, Transform)` pairs), `blend_pose`, `show_pose`, `hide_pose`, `get_pose_from_animation`), `WalkerAIController` (`start`, `stop`, `go_to_location`, `set_max_speed`) |
| Traffic signs | `TrafficSign` (`trigger_volume`), from `as_traffic_sign()` on a traffic sign or a traffic light (issue #19) |
| Traffic lights | `TrafficLight` (`state` / `get_state` / `set_state` (`TrafficLightState`), green/yellow/red times, `get_elapsed_time`, `freeze`, `is_frozen`, `get_pole_index`, `reset_group`, `get_opendrive_id`, `trigger_volume`, `get_affected_lane_waypoints`, `get_stop_waypoints`, `get_group_traffic_lights`, `get_light_boxes`) |
| Lights | `World.get_lightmanager()`, `LightManager` (`get_all_lights`, `get_turned_on_lights` / `get_turned_off_lights`, `turn_on` / `turn_off`, `set_active` / `is_active`, `set_color(s)` / `get_color`, `set_intensity` / `set_intensities` / `get_intensity`, `set_light_group(s)` / `get_light_group`, `set_light_state(s)` / `get_light_state`, `set_day_night_cycle`), `Light` (`id`, `location`, `color`, `intensity`, `light_group`, `light_state`, `is_on`, `turn_on` / `turn_off`, `set_color`, `set_intensity`, `set_light_group`, `set_light_state`), `LightGroup`, `LightState` |
| Traffic Manager | `TrafficManager` (`set_synchronous_mode`, `set_random_device_seed`, `set_hybrid_physics_mode`, `global_percentage_speed_difference`, `set_global_distance_to_leading_vehicle`, per-vehicle `vehicle_percentage_speed_difference`, `distance_to_leading_vehicle`, `random_left/right_lanechange_percentage`, `ignore_lights/signs/vehicles/walkers_percentage`, `keep_right_rule_percentage` / `keep_slow_lane_rule_percentage`, `set_desired_speed`, `vehicle_lane_offset`, `auto_lane_change`, `force_lane_change`, `update_vehicle_lights`, `get_port`, `global_lane_offset`, `collision_detection` (any actor as the other one), `set_osm_mode`, `set_respawn_dormant_vehicles`, `set_boundaries_respawn_dormant_vehicles`, `set_hybrid_physics_radius`, `set_path` (`List[Location]`), `set_route` (road option names), `get_next_action` (→ `Tuple[str, Waypoint]`), `get_all_actions` (→ `List[Tuple[str, Waypoint]]`), `shut_down`; `global_large_vehicle_wide_turn` / `vehicle_large_vehicle_wide_turn` (LibCarla ue5-dev)) |
| Weather | `WeatherParameters` (all 14 fields; LibCarla's 23 named presets as class attributes, `WeatherParameters.ClearNoon` & co., shared objects as in the Python API, and as copies through `WeatherParameters.preset("ClearNoon")`) |
| Map | `carla.Map(name, xodr_content)` (client-side, from an OpenDRIVE string), `name`, `get_spawn_points`, `get_waypoint` (→ `Optional[Waypoint]`), `get_waypoint_xodr` (→ `Optional[Waypoint]`), `generate_waypoints`, `to_opendrive`, `save_to_disk`, `cook_in_memory_map`, `get_topology`, `get_crosswalks`, `get_all_landmarks`, `get_all_landmarks_of_type`, `get_all_landmarks_from_id`, `get_landmark_group`; geo-referencing: `get_georeference`, `transform_to_geolocation`, and with CARLA ue5-dev `get_geoprojection`, `geolocation_to_transform` and explicit projections (`GeoLocation`, `GeoEllipsoid`, `GeoOffsetTransform`, `GeoProjectionTM` / `UTM` / `WebMerc` / `LCC2SP`, `GeoProjection`); `LaneType`, `Junction` (`id`, `bounding_box`, `get_waypoints`) |
| Waypoint | `id`, `transform`, `road_id`, `section_id`, `lane_id`, `s`, `is_junction`, `is_intersection`, `junction_id`, `lane_width`, `lane_type`, `lane_change` (`LaneChange`), `left_lane_marking` / `right_lane_marking` (→ `Optional[LaneMarking]`: `LaneMarkingType`, `LaneMarkingColor`), `is_rht`, `next`, `previous`, `next_until_lane_end`, `previous_until_lane_start`, `get_left_lane` / `get_right_lane` / `get_junction` (→ `Optional`), `get_landmarks`, `get_landmarks_of_type` |
| Landmark | every field of the Python API (`id`, `name`, `type`, `road_id`, `s`, `t`, `distance`, `orientation` (`LandmarkOrientation`), `h_offset`, `pitch`, `roll`, `is_dynamic`, ...), `waypoint` (→ `Optional[Waypoint]`), `get_lane_validities`; `LandmarkType` |
| Snapshots | `WorldSnapshot` (`id`, `frame` / `frame_count`, `timestamp`, `elapsed_seconds`, `delta_seconds`, `platform_timestamp`, `find` → `Optional`, `has_actor`, indexing, iteration), `ActorSnapshot`, `Timestamp` (`frame` / `frame_count`) |
| Sensors | `Actor.as_sensor()` (checked), `Sensor.listen(callback)` (dispatched on the program's thread at `tick` / `wait_for_tick` / `carla.dispatch_sensor_callbacks()`, or on the callback thread: `carla.start_callback_thread()` / `stop_callback_thread()` / `callback_thread_running()`, issue #86), `Sensor.listen(queue_size)` / `stop` / `destroy` / `poll` (→ `Optional[SensorData]`) / `wait_for_data` / `has_callback` / `pending_count` / `dropped_count`, `enable_for_ros` / `disable_for_ros` / `is_enabled_for_ros`, `listen_to_gbuffer` / `is_listening_gbuffer` / `stop_gbuffer` (`GBufferTextureID`; issue #33); `SensorData` (`frame`, `frame_number`, `timestamp`, `transform`) with checked `as_image()` / `as_lidar()` / `as_semantic_lidar()` / `as_radar()` / `as_gnss()` / `as_imu()` / `as_collision()` / `as_obstacle()` / `as_lane_invasion()` / `as_dvs()` / `as_optical_flow()`; `Image` (`raw_data` → read-only `RawData` view, zero-copy `raw_data_ptr()`, indexing and iteration → `Color`, `pixel`, `convert(ColorConverter)`, `save_to_disk(path, ColorConverter)` as PNG), `ColorConverter` (`Raw`, `Depth`, `LogarithmicDepth`, `CityScapesPalette`), `LidarMeasurement` (`raw_data`, zero-copy `raw_data_ptr()` / `raw_points()`, iteration, `get_point_count`, `save_to_disk` as PLY), `SemanticLidarMeasurement` / `SemanticLidarDetection`, `RadarMeasurement` / `RadarDetection`, `DVSEventArray` / `DVSEvent` (`to_image`, `to_array`, `to_array_x/y/t/pol`), `OpticalFlowImage` / `OpticalFlowPixel` (`get_color_coded_flow` → `FakeImage`) — all zero-copy views of LibCarla's buffers; `GnssMeasurement`, `IMUMeasurement`, `CollisionEvent` / `ObstacleDetectionEvent` (`actor`, `other_actor` → `Optional[Actor]`, `distance`), `LaneInvasionEvent` (`crossed_lane_markings` → `List[LaneMarking]`, `actor`), `LaneInvasionSensor`, `ServerSideSensor`, `ClientSideSensor`, `LaneMarking` / `LaneMarkingType` / `LaneMarkingColor` / `LaneChange`; V2X (issue #42, LibCarla ue5-dev only): `Sensor.send(CustomV2XBytes)` / `send(str)`, `as_cam_event()` → `CAMEvent` (`get_message_count`, indexing, iteration → `CAMMessage`: `power`, `get()` → typed `CAM`), `as_custom_v2x_event()` → `CustomV2XEvent` (→ `CustomV2XData`: `power`, `get()` → `CustomV2XMessage`), `CustomV2XBytes` (`get_string` / `set_string` / `get_bytes` / `set_bytes` / `data_size` / `max_data_size` / `get`); the Python API's dict keys on every `get()` result, resolved at compile time (`get()["Message"]["Message"]["DataSize"]`, issue #85) |
| Batch commands | `carla.command.SpawnActor(...).then(...)`, `FutureActor`, `DestroyActor`, `ApplyVehicleControl`, `ApplyWalkerControl`, `ApplyTransform`, `ApplyLocation`, `ApplyTargetVelocity`, `ApplyTargetAngularVelocity`, `ApplyImpulse`, `ApplyForce`, `ApplyAngularImpulse`, `ApplyTorque`, `SetAutopilot`, `SetSimulatePhysics`, `SetEnableGravity`, `SetVehicleLightState`, `SetTrafficLightState`, `ApplyVehicleAckermannControl`, `ShowDebugTelemetry`, `ApplyVehiclePhysicsControl`, `ApplyWalkerState`; `CommandResponse` |
| Blueprints | `BlueprintLibrary` (`find`, `filter`, `filter_by_attribute`, indexing, iteration), `ActorBlueprint` (`id`, `tags`, `has_tag`, `match_tags`, `has_attribute`, `get_attribute`, `set_attribute`, `len()` and iteration over its attributes), `ActorAttribute` (typed `as_bool/as_int/as_float/as_str/as_color`, `int()` / `float()` / `bool()`, `recommended_values`) |
| Values | `TextureColor`, `TextureFloatColor`, `FloatColor`, `MaterialParameter`, `CityObjectLabel`, `ActorState` (issue #19), `MapLayer`, `EnvironmentObject`, `LabelledPoint` (issue #21), `Location`, `Rotation` (`get_forward_vector` / `get_right_vector` / `get_up_vector`, `get_normalized`), `Transform` (`get_forward_vector` / `get_right_vector` / `get_up_vector`, `get_matrix`, `get_inverse_matrix`, `transform` (a point or a list of points, in place), `transform_vector`), `Vector2D` (`length`, `squared_length`, `make_unit_vector`), `Vector3D`, `Velocity`, `AngularVelocity`, `Acceleration`, `Quaternion`, `BoundingBox` (`contains`, `get_local_vertices`, `get_world_vertices`), `VehicleControl`, `VehiclePhysicsControl` and `WheelPhysicsControl` (all fields: curves, gear ratios, wheels), `WorldSettings` (every LibCarla field, including `max_culling_distance`, `deterministic_ragdolls`, `tile_stream_distance`, `actor_active_distance`, `spectator_as_ego`), `Color`, `VehicleAckermannControl`, `AckermannControllerSettings`, `VehicleTelemetryData`, `WheelTelemetryData`, `WalkerBoneControlIn` / `WalkerBoneControlOut`, `bone_transform` / `bone_transform_out`; enumerations `VehicleDoor`, `VehicleWheelLocation`, `VehicleFailureState`, `AttachmentType`; constants `GBufferTextureID` |
| Errors | `CarlaError`, `TimeoutError`, `ActorTypeError`, `VersionError` (plus `IndexError` for lookups by key or index) |
| Tooling | `typesafe-codon` launcher, `typesafe-carla-toolchain` (bundled Codon), uv workspace + `uv.lock`, CI, PyPI release workflow, binding generator ([docs/bindgen.md](docs/bindgen.md)) |

Notes on issue #22 (map, waypoint, landmark and traffic light gaps):
- **Geo projections need CARLA ue5-dev.** `get_georeference` and
  `transform_to_geolocation(location)` work with every LibCarla. CARLA 0.10.0's
  LibCarla has no `geom::GeoProjection`: there, `transform_to_geolocation`
  uses the geo-reference's Mercator approximation (`GeoLocation::Transform`),
  and `get_geoprojection`, `geolocation_to_transform` and an explicit
  projection argument raise `CarlaError`. Newer LibCarla (ue5-dev) projects
  with the map's `GeoProjection`, which treats y differently: away from the
  origin the two give different latitudes. The official `carla` 0.10.0 wheel
  on PyPI already has the newer code, so against a 0.10.0 server it can
  disagree with typesafe_carla built from the 0.10.0 tag (the compatibility
  test compares only the origin).
- `get_geoprojection()` returns a `GeoProjection`, a Union of the four
  projection classes; tell them apart with `isinstance`. Projection arguments
  are checked at compile time (a Union parameter would accept anything).
- `Waypoint.is_rht` is always `True` with CARLA 0.10.0, which has no
  left-hand traffic (its lane markings use the right-hand layout).
- `Landmark.waypoint` is `None` for landmarks from `Map.get_all_landmarks*`,
  as in LibCarla; landmarks from `Waypoint.get_landmarks*` have one.
- `TrafficLight.get_affected_lane_waypoints()` / `get_stop_waypoints()` /
  `get_group_traffic_lights()` skip the null entries LibCarla can return
  (the Python API would put `None` in the list).
- `Waypoint.get_landmarks*` with a negative or non-finite distance raises
  `CarlaError`. `Map.get_waypoint_xodr` with a road id outside uint32 or a
  lane id outside int32 returns `None` (the Python API raises `OverflowError`).
- `Map.save_to_disk` raises `CarlaError` when the file cannot be written (the
  Python API ignores it); `cook_in_memory_map`, like LibCarla, only logs it.
- `carla.Map(name, xodr_content)` (issue #39) parses the OpenDRIVE text in
  this process, with no server. Such a map has no recommended spawn points,
  as in LibCarla. Only text that does not parse as XML reliably raises:
  `CarlaError` "the OpenDRIVE document does not parse" (the Python API:
  `RuntimeError: std::exception`). LibCarla accepts any well-formed XML
  (`<foo/>` or `<OpenDRIVE/>` give an empty map); a well-formed but
  inconsistent OpenDRIVE document can raise another error (e.g. `IndexError`
  from an `out_of_range`) or crash inside LibCarla (e.g. a `<road>` without
  `<planView>`), as it does with the Python API. The mock backend has no
  OpenDRIVE parser: it accepts only a document with a closed `<OpenDRIVE>`
  element (`<foo/>` raises there) and models any such document, even
  `<OpenDRIVE/>`, as its own two-lane road.

Notes on issue #23 (Client, Traffic Manager, blueprint, debug and value-type gaps):
- **Rotation convention follows LibCarla.** CARLA ue5-dev flipped the sign of the pitch and roll terms of `Rotation`'s basis vectors and of `Transform`'s matrices relative to 0.10.0 (yaw-only rotations are the same). `get_forward_vector`, `get_right_vector`, `get_up_vector`, `get_matrix`, `get_inverse_matrix`, `transform`, `transform_vector` and `BoundingBox`'s methods take the rotation matrix from the LibCarla this library is built from, as the official Python API built from the same sources does; the rest of the math is Codon. `Quaternion` (only in ue5-dev's LibCarla and Python API) is pure Codon with ue5-dev's math, so with a LibCarla 0.10.0 build its basis vectors can differ from `Rotation`'s for a non-zero pitch or roll.
- **ue5-dev only.** `TrafficManager.global_large_vehicle_wide_turn` / `vehicle_large_vehicle_wide_turn` and `DebugHelper.clear_debug_shape` / `clear_debug_string` raise `CarlaError` with a LibCarla 0.10.0 build, which has no such functions. Clearing also needs a server with those RPCs (newer than 0.10.0).
- **Typed results.** `Client.load_world_if_different` returns the new `World`, or `None` when the map is already loaded (the Python API always returns `None`). `TrafficManager.get_next_action` returns a `(road option, Waypoint)` tuple and `get_all_actions` a list of them (Python: two-element lists). `set_route` raises `CarlaError` for an unknown road option name (Python sends an invalid code). `get_next_action` / `get_all_actions` raise `IndexError` when there is no plan (a vehicle this Traffic Manager does not drive yet, or a shut-down Traffic Manager), with every LibCarla: ue5-dev returns an empty plan there and 0.10.0 throws. They need a Traffic Manager in this process: LibCarla's `TrafficManagerServer` binds these RPCs without returning the result (an upstream bug), so a remote one cannot answer. `Transform.transform(point)` returns the same type it is given (Python: a `Vector3D`); like Python it also transforms the point in place.
- **Files.** `get_required_files(download=True)` and `request_file` write into the files base folder on this machine (`~/carlaCache/` by default, `<folder>/<LibCarla version>/<file>`), like the Python API.
- **`WorldSettings()`** has the Python API's defaults (`deterministic_ragdolls=False`, `tile_stream_distance=3000`, `actor_active_distance=2000`, `spectator_as_ego=True`); `apply_settings` sends every field, so `world.apply_settings(carla.WorldSettings(synchronous_mode=True))` also resets the server's culling and streaming fields to these defaults, as in the Python API; modify the result of `get_settings()` to keep them.
- **Constants only.** `GBufferTextureID` is defined; `Sensor.listen_to_gbuffer` and its companions take it (issue #33).
- **`AttachmentType` is a distinct type like `VehicleDoor`** (issue #34): an int is a compile error and a value outside the enumeration raises `CarlaError`, from `try_spawn_actor` too. The batch `command.SpawnActor` has no attachment type, as in the Python API.

Notes on issue #21 (World API gaps):
- **`on_tick` callbacks run on the program's thread**, like sensor callbacks:
  LibCarla's thread only queues each tick's `WorldSnapshot`, and the callback
  runs at `World.tick()`, `World.wait_for_tick()`,
  `Client.apply_batch(_sync)(do_tick=True)` and `carla.dispatch_callbacks()`
  (`dispatch_sensor_callbacks()`, the earlier name, does the same). LibCarla
  publishes a tick before it runs its OnTick callbacks, so `tick()` and
  `wait_for_tick()` first wait (within their timeout, usually microseconds)
  until the client's on_tick callbacks have received the returned frame; then
  that frame's snapshot is delivered by the same call (unless the wait times out).
  `apply_batch(do_tick=True)` does not wait. The queue is unbounded by default
  (`on_tick(cb, queue_size=n)` keeps the newest n). As in LibCarla, callbacks
  belong to the client: they keep running after `load_world`, and
  `remove_on_tick(id)` works through any World of the same Client. It drops
  the snapshots not yet delivered; an unknown id is ignored.
- **Light manager.** LibCarla (0.10.0 and ue5-dev) has `World::GetLightManager`,
  but the official CARLA 0.10.0 Python API does not expose
  `World.get_lightmanager` (0.10.0 has no night mode); here it is available.
  Lights cross the C ABI as ids; changes reach the server at the next tick.
  Bulk setters take one value per light (a length mismatch raises
  `CarlaError`; LibCarla silently uses the shorter length). Light ids are
  the server's; a light id the manager does not know raises `IndexError` and
  changes nothing (LibCarla would modify a placeholder state).
- **`get_imu_sensor_gravity` / `set_imu_sensor_gravity`** exist in LibCarla
  ue5-dev only (not in LibCarla 0.10.0's source, although the 0.10.0 Python
  wheel has them). Built against CARLA 0.10.0 they raise `CarlaError`.
- **`get_traffic_sign(landmark)` / `get_traffic_light(landmark)`** return
  `None` when no actor matches the landmark's id (`TrafficSign` /
  `TrafficLight` otherwise).
- **`get_traffic_lights_in_junction(id)`** returns an empty list for an id that
  names no junction (LibCarla dereferences a null junction there).
- **`EnvironmentObject.id`** is LibCarla's `uint64` bit for bit, so ids at or
  above 2^63 read as negative `int`s; pass them back unchanged.
- **Textures**: `get` / `set` outside the texture raise `IndexError` (LibCarla
  does not check). `FloatColor` channels are stored as float32.

Notes on Milestone 4:
- **C ABI 2.0.** `tsc_command_t` gained a field (walker speed), an incompatible change made before the first release. The Codon module checks the major version on import.
- **Weather depends on the server.** It can be disabled there: CARLA 0.10.0 (the Docker image used for testing) reports `is_weather_enabled() == False`, and `set_weather` has no effect, exactly as with the official Python API.
- **Traffic Manager.** It runs inside LibCarla in the client process. In synchronous mode, call `tm.set_synchronous_mode(True)` as well.
- **Enumerations.** `TrafficLightState`, `VehicleLightState` and `LaneType` are integer constants, as in the Python API (`VehicleLightState` values combine with `|`).
- **`names` / `values` (issue #33).** Every enumeration has the Python API's
  `names` (member name → member) and `values` (value → member; where members
  share a value, the last one declared, e.g. `LaneMarkingColor.values[0]` is
  `White`). The integer enumerations (`TrafficLightState`, `LaneType`,
  `CityObjectLabel`, ...) are each the one instance of a class whose fields are
  the members, so `names` / `values` come from one shared implementation
  (`_enum.codon`); a misspelt member is a compile error naming that class
  (`'_TrafficLightState' object has no attribute 'Purple'`). Their `names` and
  `values` build a new dict on each access; in the Python API they are shared
  class attributes. The members are the same as the official CARLA 0.10.0
  module's (checked by `tests/test_runtime.py`).

Notes on issue #20 (Vehicle and Walker API gaps):
- **Typed enumerations.** `VehicleDoor`, `VehicleWheelLocation` and `VehicleFailureState`
  are distinct types, not `int` constants (unlike `TrafficLightState` and the bit-flag
  enumerations): `vehicle.open_door(carla.VehicleWheelLocation.FL_Wheel)` and
  `vehicle.open_door(0)` do not compile. The official API rejects an `int` there too.
  `int(carla.VehicleDoor.All)` gives the value. `VehicleDoor` has the Python API's
  members (`FL`, `FR`, `RL`, `RR`, `All`); LibCarla's `Hood` and `Trunk` have no named
  members, as in the Python API; their raw values 4 and 5 (`carla.VehicleDoor(4)`) pass through.
- **Depends on the LibCarla version.** `Vehicle.get_telemetry_data()` and
  `Vehicle.get_vehicle_bone_world_transforms()` call LibCarla methods that ue5-dev has
  and CARLA 0.10.0 does not. Built against LibCarla 0.10.0, they raise `CarlaError`
  ("... is not available in LibCarla 0.10.0"); the rest of the library is unaffected.
- **Recorder and replayer arguments (issue #36).** `Client.replay_file(name, time_start,
  duration, follow_id, replay_sensors, replay_weather, offset, map_override)` and
  `Client.start_recorder(name, additional_data, stop_replayer)` have the official
  arguments and keywords (`show_recorder_*` take `name`, and `show_recorder_collisions`
  `type1` / `type2`). `replay_weather`, `offset` and `map_override`, and `stop_replayer`,
  exist only in LibCarla ue5-dev: while they keep their defaults (`False`, `Transform()`,
  `""`; `True`) the call is the one LibCarla 0.10.0 also has, so they work everywhere;
  another value needs ue5-dev, and built against 0.10.0 raises `CarlaError`
  ("... is not available in LibCarla 0.10.0"). `offset` may also be `None` (the default).
- **Server plugins.** `enable_carsim`, `use_carsim_road` and `enable_chrono_physics`
  are bound (one LibCarla call each), but they only do something on a server built with
  the CarSim or Chrono plugin, which CARLA UE5 does not ship. LibCarla sends them
  asynchronously, so an unsupported server ignores them without an error.
  `enable_chrono_physics`'s fourth keyword is `powetrain_json`, spelled as in the
  official Python API. `restore_physx_physics` is not bound: neither LibCarla 0.10.0
  nor ue5-dev has a counterpart (the official module lists it, but there is no
  LibCarla method to call).
- **Fire and forget.** `open_door`, `close_door`, `set_wheel_steer_direction`,
  `show_debug_telemetry`, `apply_ackermann_controller_settings`, `set_bones`,
  `blend_pose` / `show_pose` / `hide_pose` and `get_pose_from_animation` are
  asynchronous RPCs in LibCarla: the server's errors (an unknown bone name, a door a
  vehicle does not have) are not reported. `set_wheel_steer_direction` turns the wheel's
  bone only; `get_wheel_steer_angle` reports the physics angle.

Notes on Milestone 5:
- **Generated plumbing, hand-written API.** 252 C ABI functions are generated from
  `bindings/*.yaml`: the C declarations, the C++ shim and the Codon FFI. Each one is a handle check,
  argument conversions and a single LibCarla call. The ABI is unchanged; libclang compared every
  prototype and struct size before and after the migration. See [docs/bindgen.md](docs/bindgen.md).
- **Validated against LibCarla.** `tools.bindgen validate` parses the shim with libclang
  and checks each spec'd method's existence, arity and types. It runs against the mock
  headers and against LibCarla ue5-dev and 0.10.0 in CI.
- **Coverage.** [docs/coverage.md](docs/coverage.md) lists the public methods of the main
  LibCarla client classes and whether the shim calls them (generated, hand-written or not yet).

Notes on Milestone 2:
- **No callbacks on LibCarla threads (design §15).** LibCarla's threads only fill a per-sensor queue. `listen(callback)` runs the callback on the program's own thread, at `World.tick()`, `World.wait_for_tick()`, `Client.apply_batch(_sync)(do_tick=True)` and `carla.dispatch_sensor_callbacks()`; or, after `carla.start_callback_thread()` (issue #86), on one background thread as soon as data arrives, as the Python API does. Its queue is unbounded by default. `stop()`, `destroy()` (through any handle) and batch `DestroyActor` unregister it. `listen()` / `listen(queue_size)` is polling mode: the program reads a bounded queue (64 by default) with `poll()` / `wait_for_data()`, and the oldest measurement is dropped when it is full (`dropped_count`). `queue_size=0` means unbounded in both modes. Call `stop()` (or destroy the sensor) when done.
- **Zero copy (design §16).** `Image.raw_data_ptr()` (BGRA) and `LidarMeasurement.raw_points()` point into LibCarla's buffer and stay valid while the measurement object is alive. The `raw_data` property (issue #78) is a read-only view of the same bytes that keeps the measurement alive itself.
- `World.spawn_actor(..., attach_to=...)` accepts any actor subclass (e.g. a `Vehicle`) or an `Optional` of one, and rejects non-actors at compile time.

Notes on Milestone 1:
- **Physics control.** `VehiclePhysicsControl` and `WheelPhysicsControl` have every field of LibCarla UE5's `rpc::VehiclePhysicsControl` / `rpc::WheelPhysicsControl` (identical in 0.10.0 and ue5-dev), with the Python API's names and LibCarla's defaults: the torque and steering curves and each wheel's `lateral_slip_graph` (`List[Vector2D]`), `forward_gear_ratios` / `reverse_gear_ratios` (`List[float]`; the official 0.10.0 Python API cannot read or set these), the engine, transmission, chassis and suspension scalars, and the `uint8_t` codes (`differential_type`, `axle_type`, ...) as `int` in [0, 255]. The list fields are values, as in the Python API: `pc.wheels`, `pc.torque_curve`, ... return a new list of copies on each read, so change the list and assign it back (`wheels = pc.wheels; wheels[0].wheel_radius = 40.0; pc.wheels = wheels`); `pc.wheels[0].wheel_radius = 40.0` changes only a copy. `apply_physics_control` reads the vehicle's current control and overwrites every field, so fields a newer LibCarla adds keep the server's values; the wheel count must match the vehicle's. CARLA 0.10.0 applies changes a few frames later, ignores per-wheel fields such as `max_brake_torque`, `max_steer_angle`, `wheel_radius` and `cornering_stiffness`, and puts two default keys in front of every curve it is given (so each apply grows `torque_curve` and `steering_curve` by two points), all exactly as the official Python API reads back.
- **Batch commands.** As in the Python API, a command's actor (and `SpawnActor`'s parent) is an `Actor` of any kind or its id, given positionally or by the official keywords `actor_id=` / `actor=` (`parent_id=` / `parent=`); passing both, or neither, is a compile error. An `Optional` actor such as `world.get_actor(id)` is accepted too and raises `CarlaError` when it is `None` at run time; a literal `None` (on which the official 0.10.0 module crashes), an `Optional[int]` or any other type is a compile error. Unlike the official overloads, either keyword takes either form (`actor_id=vehicle` works). Every constructor returns one `Command` type, so one list can mix command kinds. `SetAutopilot` in `apply_batch_sync` also registers the vehicle with the Traffic Manager, like the Python API. `ApplyVehiclePhysicsControl` copies the control when the command is made and sends all of it; unlike `Vehicle.apply_physics_control()`, it cannot check the wheel count against the vehicle's.

### Supported CARLA versions

**CARLA UE5 only** (the CMake-based `ue5-dev` branch, 0.10.x and later).
UE4 (0.9.x, `ue4-dev`) is not supported.

LibCarla is built from CARLA sources as part of the build. The default is the
latest `ue5-dev`; any branch, tag or commit SHA can be selected:

| How | Example |
|---|---|
| environment variable | `CARLA_GIT_REF=0.10.0` |
| CMake | `-DTSC_CARLA_GIT_REF=<branch\|tag\|sha>` |
| pip / uv build setting | `-C cmake.define.TSC_CARLA_GIT_REF=<ref>` |
| local checkout | `CARLA_SOURCE_DIR=~/carla` or `-DTSC_CARLA_SOURCE_DIR=...` |
| other repository (fork) | `-DTSC_CARLA_GIT_REPOSITORY=https://github.com/<you>/carla.git` |
| ref name to record for a SHA | `-DTSC_CARLA_REF_NAME=ue5-dev` (or `CARLA_REF_NAME`) |

Released wheels name their CARLA ref in the release tag,
`<version>-<CARLA ref>-<YYYYMMDD>` (e.g. `0.1.0-ue5-dev-20260915`: `ue5-dev`
as of its last commit on that day); see [docs/releasing.md](docs/releasing.md).

Only `CMakeLists.txt`, `CMake/` and `LibCarla/` are fetched, as a shallow,
blob-filtered, sparse checkout (a few MB, not the multi-GB repository). A
moving branch is re-fetched only on `-DTSC_CARLA_REFRESH=ON`. The ref and the
resolved commit are compiled in: `typesafe-codon info`,
`carla.libcarla_git_ref()` / `carla.libcarla_git_commit()` in Codon, and
`_native/BUILD_INFO.json` in the wheel. When the ref is a commit SHA resolved
from a branch or tag (CI and releases build that way), `TSC_CARLA_REF_NAME`
names that branch or tag, and it is what is recorded as the ref: e.g.
`ue5-dev` with commit `1360bb9…`.

| typesafe_carla | ABI | Codon | Python | CARLA | Platform | Tested |
|---|---|---|---|---|---|---|
| 0.1.0 | 4.5 | 0.19.x | ≥ 3.10 (launcher only) | UE5: `ue5-dev` (default), `0.10.0` | Linux x86_64; Ubuntu 22.04 (GCC 11), 24.04 (GCC 13), 26.04 (GCC 15) | `0.10.0`: integration and compatibility tests pass against a CARLA 0.10.0 server. `ue5-dev` and `0.10.0`: build, link and pass the C ABI tests in CI on each of those Ubuntu LTS releases with its default GCC |

### Backends

`TSC_BACKEND` selects the implementation behind the C ABI. The shim in
`native/src` is shared by both:

* **`libcarla`** (default): LibCarla built from CARLA UE5 sources as above,
  linked statically into `libtypesafe_carla_ffi.so`. The resulting library
  depends only on libstdc++/libc and exports only the `tsc_*` functions.
* **`mock`**: an in-memory stand-in for LibCarla (`native/mock`) with the same
  class and method signatures, a fake "server" per `host:port`, and a toy
  vehicle model. It is used for the compile and runtime test suites and for
  development without CARLA. It is not a simulator.

`typesafe-codon info` and `typesafe_carla.backend()` report which one you
have.

## Installation

See [docs/usage.md](docs/usage.md) for using typesafe_carla from your own
project. Once it is published:

```sh
uv add typesafe-carla        # or: pip install typesafe-carla
uv run typesafe-codon run main.py
```

Python 3.10 or newer (CI tests 3.10 and 3.14). Python only runs the
`typesafe-codon` launcher; your programs are compiled by Codon.
`typesafe-carla` depends on `typesafe-carla-toolchain`, which bundles a
pinned Codon. Requirements and building for another CARLA ref are in
[docs/usage.md](docs/usage.md); the release process is in
[docs/releasing.md](docs/releasing.md).

### From CPython: `import typesafe_carla.carla as carla`

CPython programs (a scenario framework, a test harness) use the same library
with the CARLA Python API's names, in place of the `carla` package:

```python
import typesafe_carla.carla as carla

client = carla.Client("localhost", 2000)
world = client.get_world()
```

`typesafe_carla.carla` is the library compiled with `codon build --pyext`
(`typesafe_carla.pycarla`; CARLA's own PythonAPI tests run unmodified on it,
see below). **The released wheel carries it prebuilt**
(`typesafe_carla/carla/_prebuilt`, added by the release workflow): `pip
install typesafe-carla` imports it with nothing to compile. One build serves
every Python 3.10+: Codon's output does not depend on the Python version, and
the package finds the native library and the Codon runtime at run time.

Where no prebuilt package matches the installation (a source checkout, an
sdist install, edited Codon sources, another `typesafe-carla-toolchain`
release), the first import builds it instead, into
`~/.cache/typesafe_carla/pycarla/<key>`; that takes 15 to 50 minutes and ~14 GB
of memory and needs `cc`. `typesafe-codon pycarla` does the same ahead of time
(and says so when the wheel's build already applies).
`TYPESAFE_CARLA_PYCARLA_DIR` chooses the build directory (and wins over the
prebuilt package), and `TYPESAFE_CARLA_PYCARLA_BUILD=0` makes a missing build
an `ImportError` instead of a build (`python/typesafe_carla/carla_build.py`).
It does not take the `carla` import name, so it can sit next to the official
package.

The same import compiles with Codon (`codon/typesafe_carla/carla.codon` is the
library under that name), so one source both runs on CPython and is checked by
the Codon compiler. Codon 0.19 resolves the module only in this form, not as
`from typesafe_carla import carla`.

## Testing against a real CARLA server

`tests/integration/*.codon` and `tests/compatibility/` need a running CARLA
UE5 server and the `libcarla` backend built from the matching ref (a
`0.10.0` server needs `-DTSC_CARLA_GIT_REF=0.10.0`):

```sh
cmake -S . -B build-carla -DTSC_CARLA_GIT_REF=0.10.0 && cmake --build build-carla -j
export TSC_CARLA_HOST=localhost TSC_CARLA_PORT=2000 TYPESAFE_CARLA_BUILD_DIR=build-carla
uv run pytest tests/test_integration.py -s
# Same scenario through the official Python API and typesafe_carla, compared:
CARLA_PYTHON=/path/to/venv-with-carla/bin/python uv run python tests/compatibility/compare.py
```

### CARLA's own PythonAPI tests

CARLA's test suite (`PythonAPI/test`: `unit`, `smoke`, `API` and the
top-level files; the few that need a map no ue5-dev package ships run as
ports under `tests/upstream/ported`, and interactive scripts are skipped)
runs against typesafe_carla, from the CARLA commit
the native library was built from, in two modes:

- **cpython**: the tests run unmodified under CPython, with `import carla`
  being typesafe_carla built as a CPython extension (`tools/pycarla.py`,
  `codon build --pyext`);
- **codon**: the tests are converted mechanically and compiled with
  `typesafe-codon`.

[tests/upstream/expectations.yaml](tests/upstream/expectations.yaml)
records the expected result per mode, CARLA ref and test. pytest and CI fail
on an unexpected failure and on an unexpected pass, so that list only shrinks.
[tests/upstream/GAPS.md](tests/upstream/GAPS.md) groups the failures by root
cause, separating missing typesafe_carla features from harness and Codon
limits. `unit` needs no server and runs in pytest and in CI: codon mode on
every `libcarla` leg, cpython mode on one leg per CARLA ref. The others run
only with `TSC_CARLA_PORT` set. See
[tests/upstream/README.md](tests/upstream/README.md).

```sh
uv run python -m tools.upstream_tests --mode all --suite unit -v
TSC_CARLA_HOST=127.0.0.1 TSC_CARLA_PORT=2000 TYPESAFE_CARLA_BUILD_DIR=build-carla \
  uv run python -m tools.upstream_tests --mode all --suite smoke --suite API --suite top -j1 -v
```

## Quick start (development)

Requirements: Linux x86_64, a C++20 compiler (GCC ≥ 11; CI builds with the default GCC of
Ubuntu 22.04, 24.04 and 26.04), git, CMake ≥ 3.27.2, and uv.
Codon is installed by `uv sync` from the `toolchain/` workspace member.

```sh
TSC_BACKEND=mock uv sync                # fast: editable install with the mock backend
uv run typesafe-codon info

# Mock backend: everything the test suites need.
cmake -S . -B build -DTSC_BACKEND=mock && cmake --build build -j
ctest --test-dir build                  # C ABI tests
uv run pytest                           # compile-pass/fail, runtime and launcher tests
uv run typesafe-codon run examples/connect.py

# Real backend: LibCarla from CARLA ue5-dev (first build fetches and compiles
# LibCarla and its dependencies; takes a while).
cmake -S . -B build-carla -DTSC_CARLA_GIT_REF=ue5-dev && cmake --build build-carla -j
TYPESAFE_CARLA_BUILD_DIR=build-carla uv run typesafe-codon run examples/connect.py
```

If GitHub archive downloads are blocked by your network but git works, add
`-DPREFER_CLONE=ON`; CARLA then clones its dependencies instead.

To skip compiling LibCarla, add `-DTSC_CARLA_PREBUILT=auto`: CMake then
downloads the LibCarla that CI built for the same CARLA commit and the same
compiler (Ubuntu 22.04/GCC 11, 24.04/GCC 13 or 26.04/GCC 15, from a run on
`main`), and only compiles the shim. This needs an authenticated
[`gh`](https://cli.github.com/) (`gh auth login`), because GitHub serves
workflow artifacts only with a token. If there is no matching prebuilt, CMake
builds LibCarla from source as usual. `-DTSC_CARLA_PREBUILT_DIR=<prefix>` uses
a prefix you already have, e.g. from `tools/fetch_libcarla_prebuilt.py <ref>`
or `cmake --build <build dir> --target libcarla_prebuilt`. Downloads are
cached (about 155 MB each) in `~/.cache/typesafe-carla/libcarla-prebuilt`
(or under `$TYPESAFE_CARLA_CACHE_DIR`) and removed after 30 days unused;
`tools/fetch_libcarla_prebuilt.py --prune` empties it. See
[docs/releasing.md](docs/releasing.md#libcarla-prebuilt).

`typesafe-codon` passes its arguments to `codon` after setting `CODON_PATH`
(the Codon sources), `TYPESAFE_CARLA_LIB` (the native library) and
`LD_LIBRARY_PATH`. For `build`, it also gives the executable an RPATH to the
native library, so the result runs without the launcher.

## Continuing a cloud session locally

Claude Code sessions on the web can be pulled into a local terminal on any
machine. Run this from a checkout of this repository, logged in to the same
claude.ai account:

```sh
claude --teleport <session-id>    # or plain `claude --teleport` for a picker
```

Teleport fetches and checks out the session's branch (it must be pushed) and
restores the conversation. The working tree must be clean; you are prompted
to stash otherwise. It requires claude.ai login (not an API key). Project
context for new sessions is in `CLAUDE.md`.

## Repository layout

```
cmake/FetchCarla.cmake     fetches CARLA UE5 sources at a ref (default ue5-dev)
codon/typesafe_carla/      Codon API (what users import)
  _ffi.codon               raw C declarations, POD mirrors, handle ownership
native/include/.../ffi.h   the C ABI
native/src/                C ABI implementation over LibCarla
native/mock/               in-memory LibCarla stand-in (mock backend)
python/typesafe_carla/     typesafe-codon launcher, path and toolchain discovery
toolchain/                 typesafe-carla-toolchain: pinned Codon as a wheel
tools/check_wheel.py       release checks on a built wheel
tools/release_tag.py       release tags <version>-<CARLA ref>-<YYYYMMDD>
tools/bump_version.py      bumps the package version (Python and Codon)
tools/bindgen/             binding generator, libclang spec validation, coverage
bindings/                  binding spec (YAML) for the generated C ABI functions
native/src/generated/      generated C++ shim (do not edit)
.github/workflows/         CI (mock + LibCarla builds) and PyPI release
tests/native/              C ABI tests (ctest)
tests/compile/pass|fail/   programs that must / must not compile
tests/compile/strict_fail/ programs that compile only outside strict mode
tests/unit/                Codon runtime tests against the mock backend
tests/upstream/            CARLA's own PythonAPI tests: expectations, unittest shim
tools/upstream_tests.py    runs those tests (cpython and codon modes), writes GAPS.md
tools/pycarla.py           the checkout's `carla` build for them (typesafe_carla.pycarla, --package carla)
python/typesafe_carla/pycarla/  typesafe_carla as a CPython package (codon --pyext): generator, runtime, pruned-overload cache
python/typesafe_carla/carla/    `import typesafe_carla.carla as carla` (built on first use, carla_build.py)
examples/                  example programs
```

## Writing new scenarios: use the statically checked style

typesafe_carla has two ways to write the same thing. The **compatibility**
style exists so that code written for the CARLA Python API ports with few
changes. The **statically checked** style is what the type checker can verify
at compile time. **For new scenarios, write the statically checked style, and
enforce it with strict mode.**

| | Compatibility style (ported code) | Statically checked style (recommended) |
|---|---|---|
| Vehicle methods on an actor from `get_actor` | `world.get_actor(i).apply_control(c)`: the actor kind is checked at run time (`ActorTypeError`) | `world.get_actor(i).as_vehicle().apply_control(c)`, or keep the `Vehicle` from `spawn_actor(...).as_vehicle()` |
| A position where a vector is expected | `actor.set_target_velocity(target_location)`: compiles, and a position silently becomes a velocity | `actor.set_target_velocity(target_location.as_vector())`, or compute a real `Vector3D` |
| A vector where a position is expected | `carla.Transform(loc + offset, rot)` | `carla.Transform(carla.Location(loc + offset), rot)` |

Use the statically checked style even where the compatibility style happens to work. The
compatibility style moves mistakes that would be compile errors (the wrong
actor kind, a position passed as a velocity) to run time, or makes them
silent.

**Warnings.** Every compatibility path a program uses is reported twice. At
compile time, `typesafe-codon` lists every one the program contains before it
runs:

```
typesafe-codon: compile-time warning: Actor.apply_control(VehicleControl) without as_vehicle(): Python-API compatibility path, not statically checked (use --strict to make this an error)
```

At run time, each one is reported once, when it is first taken:

```
typesafe_carla: warning: Actor.apply_control(VehicleControl) without as_vehicle() is a Python-API compatibility path that is not statically checked; call as_vehicle() first (...)
```

`TYPESAFE_CARLA_COMPAT_WARNINGS=0` silences both, e.g. while porting a large
script. A program that uses only the statically checked style gets no
warnings at all.

**Strict mode** turns every compatibility path into a compile error; the last line of the
trace is the line using it:

```sh
uv run typesafe-codon --strict run main.py           # or: build
TYPESAFE_CARLA_STRICT=1 uv run typesafe-codon run main.py
```

```
_strict.codon:26 (9-108): error: strict mode: call as_vehicle() first (Actor.apply_control is a Python-API compatibility shortcut)
├─ _actor_compat.codon:46 (9-24): error: during the realization of compat_shortcut(...)
╰─ main.py:7 (1-52): error: during the realization of apply_control(self: Actor, control: VehicleControl, S: Actor)
```

Enable it in CI for new projects (`TYPESAFE_CARLA_STRICT=1`), so code stays in
the statically checked style. Strict mode only adds errors: a program that
compiles in strict mode behaves the same without it. Some things are checked
in every mode, strict or not:
- a typed actor never gets another kind's methods (`vehicle.listen()` does
  not compile);
- argument types are always checked (`apply_control(carla.Transform())` does
  not compile).

## Differences from the CARLA Python API

Most code ports by changing `import carla` to `import typesafe_carla as carla`.
Keyword arguments use the official parameter names (issue #41), so calls
with keywords port too: `TrafficManager`'s per-vehicle methods take `actor`
(and `perc` or `percentage`, as each official method does),
`set_random_device_seed(value)`, `set_synchronous_mode(mode_switch)`,
`TrafficLight.set_green_time(green_time)` & co., `ActorList.find(id)`,
`Vector3D`'s methods `vector`, `Transform.transform_vector(in_point)`. Where
the official binding leaves the first argument positional-only and gives
its name to the next one, so do these: `BoundingBox.contains(world_point,
point)` (`point` is the Transform) and
`Vehicle.set_wheel_steer_direction(wheel, wheel_location)` (`wheel_location`
is the angle). The names used before issue #41 (`vehicle=`, `percentage=`
where the official name is `perc`, `seed=`, `enabled=` for
`set_synchronous_mode`, `seconds=`, `other=`, `actor_id=` for
`ActorList.find`, `in_vector=`, `bbox_transform=`, `angle_in_deg=`) are now
compile errors ("... is an invalid keyword argument").

Deliberate differences, all in favour of static checking:

* **Subclass methods on a plain `Actor` are checked at run time.** As in the
  Python API, `world.get_actor(id).apply_control(...)` works. The Vehicle,
  Walker, WalkerAIController, TrafficLight, TrafficSign and Sensor methods on `Actor`
  convert with the matching `as_*()` and raise `ActorTypeError` when the actor
  is of another kind; argument types are still checked at compile time. Each
  such shortcut warns once at compile time (launcher) and at run time
  (`TYPESAFE_CARLA_COMPAT_WARNINGS=0` silences both); `typesafe-codon --strict`
  makes them compile errors. The static path is `as_vehicle()` & co., which
  return a typed `Vehicle`, `Walker`, ... Limits: `Actor.get_control()`
  returns a `VehicleControl` (a walker's needs `as_walker().get_control()`).
  Only a plain `Actor` has the shortcuts: on a typed subclass, another kind's
  method is a compile error in every mode (`vehicle.listen()`: "Vehicle has
  no method listen()"), as in the Python API, where `carla.Vehicle` has no
  `listen`.
* **A `Vector3D` does not become a `Location` on assignment to a variable.**
  As in CARLA 0.10.0, `Location` arithmetic gives a `Vector3D`, and the
  Python API converts it back implicitly. API parameters and field setters
  do the same here, with a warning (see docs/usage.md): `t.location = loc +
  offset` and `t.location /= k` store a `Location`, as in Python. A
  variable cannot convert, because Codon has no hook for it and a variable
  keeps one static type: a rebound local (`loc = actor.get_location()`
  followed by `loc = loc + offset` in a loop, or a conditional
  `loc = loc + offset`) does not compile (`'Vector3D' does not match
  expected type 'Location'`). Write `loc += offset` (in place: `loc` stays a
  `Location`, as in Python) or `loc = carla.Location(loc + offset)`.
* **No `/=` on a `Location`.** The Python API has no in-place `/=`, so
  `loc /= k` rebinds `loc` to the `Vector3D` `loc / k`. A variable keeps one
  static type here, so on a `Location`, `Velocity`, `AngularVelocity` or
  `Acceleration` variable it is a compile error (`'Vector3D' does not match
  expected type 'Location'`); write `loc *= 1.0 / k` (in place) or
  `v = loc / k`. On a `Vector3D` or `Vector2D` it works as in Python, and on
  a field (`t.location /= k`) the setter converts, as above.
* **Vector arithmetic otherwise matches the Python API.** Arithmetic across `Vector3D`, `Location`, `Velocity`,
  `AngularVelocity`, `Acceleration` and `Vector2D` matches the Python API
  (`tests/compatibility/arithmetic_cases.py`): `+` and `-` mix any of the
  Vector3D family and give a `Vector3D`; `*` and `/` by a scalar give a
  `Vector3D` (a `Vector2D` on a `Vector2D`), and `k / v` is `v / k`, as
  LibCarla computes it; `+=`, `-=` and `*=` update the left operand in place
  and keep its type (`t.location += v` changes `t`). Mixing a `Vector2D`
  with the Vector3D family, or any other type (`loc + rotation`), is a
  compile error. One difference: a `bool` scalar (`v * True`) does not
  compile.
* **Value semantics.** The Python API's vectors and transforms are C++
  values, and typesafe_carla copies where it does: constructors and field
  setters store copies (`t.location = loc`, then `loc += v`, leaves `t`
  alone; a setter accepts the other vector type with a warning, as the
  constructor does), and getter methods and read-only properties return copies
  (`ActorSnapshot.get_velocity()`, `Light.location`, `Landmark.transform`,
  `IMUMeasurement.accelerometer`, ...). Field getters return the stored value,
  as the Python API's do, so `t.location.x = 1` and `t.location += v` change
  `t`. List fields are values too (issue #84): the constructor and the
  setter store a copy of the list and of each element, so changing the list
  afterwards leaves the object alone. Their getters follow the Python API:
  `VehiclePhysicsControl`'s lists, `WalkerBoneControlIn/Out.bone_transforms`
  and `VehicleTelemetryData.wheels` return a new list of copies on each read
  (change it and assign it back), while `WheelPhysicsControl.lateral_slip_graph`
  returns the stored list, as a field. Commands copy their control when they
  are made (`ApplyVehicleControl`, `ApplyWalkerControl`,
  `ApplyVehicleAckermannControl`, `ApplyVehiclePhysicsControl`), and
  `EnvironmentObject(...)` copies its transform and bounding box.
* **`get_landmarks_of_type(distance, type)`**: pass the type by position.
  Codon 0.19 cannot compile these methods with a parameter named `type`, so
  it is `landmark_type` (as in `Map.get_all_landmarks_of_type`).
* **`distance` takes either keyword.** The Python API names the argument
  `vector` in `Vector3D.distance` and `location` in `Location.distance`.
  Codon dispatches a `Location` override virtually, which rejects keywords,
  so here `distance` accepts `vector=` or `location=` (exactly one) on any
  vector, a superset of the Python API.
* **Attribute values are typed.** `ActorAttribute.as_int()` raises when the
  attribute is not an int. Read values with `as_int()`, `as_float()`,
  `as_bool()`, `as_color()` or `as_str()`: `str(attribute)` gives the Python
  API's text, `ActorAttribute(id=number_of_wheels,type=int,value=4(const))`
  (since issue #71; it used to give the raw value). `int(attribute)`,
  `float(attribute)` and `bool(attribute)` are `as_int()`, `as_float()` and
  `as_bool()`, as the Python API's `__int__` / `__float__` / `__bool__`
  (`As<int>` / `As<float>` / `As<bool>`; issue #102). Like there, they raise
  `CarlaError` for another attribute type, and so does truthiness:
  `if attribute:` raises for an attribute that is not a bool (check an
  `Optional[ActorAttribute]` with `is None`).
* **Sensor callbacks run at dispatch points by default, not on CARLA's
  threads** (see the callback thread below for the Python API's model).
  `sensor.listen(lambda data: ...)` works, but the callback receives a
  `SensorData` (convert it with `as_image()` etc.; a callback typed
  `(image: carla.Image)` does not compile) and runs on the program's own
  thread: inside `World.tick()`, `World.wait_for_tick()`,
  `Client.apply_batch_sync(..., do_tick=True)` (after the server has
  answered), `Client.apply_batch(..., do_tick=True)` (right after sending: it
  does not wait), or when the program calls
  `carla.dispatch_sensor_callbacks()` (from one thread only). A blocking
  loop that never reaches one of these never sees its callbacks run. In
  synchronous mode a measurement of frame N may reach the client just after
  `tick()` returned N; it is then delivered at the next dispatch point, so
  call `dispatch_sensor_callbacks()` (in a short wait loop if needed) when a
  frame's data is required right after its tick. An exception raised by a
  callback propagates out of the call that dispatched it, after the other
  sensors' callbacks have run. `stop()`, `destroy()` through any handle, or a
  batch `DestroyActor` unregisters the callback; stop or destroy callback
  sensors before the program exits. Callback mode
  queues without bound by default (`listen(cb, queue_size=n)` bounds it);
  `poll()` / `wait_for_data()` are for polling mode only.
* **The callback thread (issue #86) is opt-in.** In the Python API, sensor
  and `on_tick` callbacks run on LibCarla's streaming threads, so a program
  can tick and then block on its own queue (`queue.get(True, 10.0)`) while
  the data arrives. Here that pattern starves unless the program calls
  `carla.start_callback_thread()`. Then one background thread runs every
  callback as soon as its data is queued; `tick()` and the other dispatch
  points no longer run callbacks, and `dispatch_callbacks()` returns 0.
  `carla.stop_callback_thread()` returns to dispatch points (undelivered
  data is kept); the thread is also stopped at exit (a callback still running
  2 s later is reported and abandoned). The switch is process-wide (the
  callback registry is), not per client. Unlike the Python API: callbacks run
  one at a time, on that one thread (not concurrently with each other); per
  stream in arrival order; `stop()` / `destroy()` / `listen()` wait for the
  callback in progress, so no callback of the stream starts after they
  return (do not make a callback wait for the main thread while it may be
  stopping a sensor: deadlock; and do not listen/stop a sensor from a
  callback while the main thread destroys it); an exception in a callback is printed to
  stderr and delivery goes on.

  **Codon has no GIL, so callbacks race with the main thread.** Every access
  to data shared with a callback, reads included (`len`, indexing, iterating
  a list a callback appends to), must hold the same `threading.Lock`; an
  unguarded `List.append` from two threads corrupts memory. Codon 0.19 has no
  `queue.Queue` or `threading.Condition`, so a blocking get polls with a
  short sleep:

  ```python
  class SensorQueue:
      lock: threading.Lock
      items: List[carla.SensorData]
      def __init__(self):
          self.lock = threading.Lock()
          self.items = []
      def put(self, data: carla.SensorData):   # sensor.listen(q.put)
          with self.lock:
              self.items.append(data)
      def get(self, timeout: float) -> carla.SensorData:
          deadline = time.time() + timeout
          while True:
              with self.lock:
                  if len(self.items) > 0:
                      return self.items.pop(0)
              if time.time() > deadline:
                  raise carla.TimeoutError("no sensor data")
              time.sleep(0.001)
  ```

  That is why it is off by default: existing programs whose callbacks append
  to unguarded lists stay correct. `TYPESAFE_CARLA_CALLBACK_THREAD=1` starts
  the thread at the first callback registration (`listen(callback)`,
  `listen_to_gbuffer`, `on_tick`) **without changing the program: it makes an
  unchanged program multithreaded**, so set it only for programs whose
  callbacks follow the rule above. An explicit `start_callback_thread()` or
  `stop_callback_thread()` overrides it.

  A program (or wrapper) may instead run its own dispatcher thread:
  `carla.set_auto_dispatch(False)`, then loop on `seen =
  carla.queue_signal_count()`, `carla.dispatch_callbacks()`,
  `carla.wait_queue_signal(seen, timeout)`; the registry is then locked as
  with the callback thread. A thread not created by Codon (e.g. a CPython
  thread) calls `carla.attach_current_thread()` first. pycarla, serialized
  by the GIL, passes `set_auto_dispatch(False, lock=False)`: then `stop()`
  does not wait for a callback already running, and
  `start_callback_thread()` / `TYPESAFE_CARLA_CALLBACK_THREAD` are refused
  or ignored.
* **`TrafficLight` is not statically a `TrafficSign`.** In the Python API
  `carla.TrafficLight` derives from `carla.TrafficSign`. Here both derive from
  `Actor` (see [Codon limitation 9](#codon-limitations-found-while-building-this)):
  a `TrafficLight` has `trigger_volume` itself, `is_traffic_sign()` is true
  for it, and `light.as_traffic_sign()` converts it where a `TrafficSign` is
  expected (`isinstance(light, carla.TrafficSign)` is false).
* **Actor skeleton queries need LibCarla newer than CARLA 0.10.0.**
  `get_bone_names`, `get_bone_world_transforms`, `get_bone_relative_transforms`,
  `get_component_names`, `get_component_world_transform`,
  `get_component_relative_transform`, `get_socket_names`,
  `get_socket_world_transforms` and `get_socket_relative_transforms` are in
  LibCarla `ue5-dev` but not in the `0.10.0` tag, and a CARLA 0.10.0 server
  does not implement their RPCs. Built against 0.10.0 they raise `CarlaError`
  (the official 0.10.0 wheel has the methods, but the 0.10.0 server rejects
  them). Against a `ue5-dev` server they work; an unknown component name, or
  bones of an actor without a skinned mesh, raise `CarlaError` as the server
  reports them.
* **`TextureColor.get` / `set` check the pixel position** (`IndexError`);
  the Python API reads or writes out of bounds.
* **`World.on_tick` callbacks also run at dispatch points**, with the same
  rules as sensor callbacks (above): the callback receives a `WorldSnapshot`
  (a callback typed for anything else does not compile) on the program's own
  thread, not on LibCarla's. `tick()` / `wait_for_tick()` wait until the
  returned frame's snapshot is queued before dispatching it.
* **`apply_*_texture_to_objects([])` raises `CarlaError`** for an empty name
  list; the Python API sends a request that does nothing.
* **`WheelPhysicsControl.velocity` is a `Vector3D`.** LibCarla and the Python
  API store it as a `Location`; positions (`location`, `old_location`,
  `center_of_mass`) stay `Location`.
* **Float precision (issue #81).** As in LibCarla and the Python API,
  `Vector3D`, `Location`, `Velocity`, `AngularVelocity`, `Acceleration`,
  `Vector2D`, `Rotation` and `Quaternion` store float32: each field is
  rounded to float32 on construction, on assignment and for every arithmetic
  result, so `Location(0.1).x` is `0.10000000149011612`,
  `Location(16777217).x` is `16777216.0` and `Vector3D(1e30) * 1e10` is
  `inf`. The fields are Codon `float`s holding float32 values; reading them
  costs nothing and the C ABI is unchanged. `+ - * /` (the scalar rounded to
  float32 first, as LibCarla's `float` parameter), `length`,
  `squared_length`, `dot`, `cross`, `distance*`, `make_unit_vector` and
  `get_vector_angle`, and `Quaternion`'s `length`, `inverse`,
  `unit_quaternion`, `*` and basis vectors, compute step by step in float32,
  as LibCarla, and give the Python API's results bit for bit
  (tests/compatibility/arithmetic_cases). So do the derived geometry
  (issue #90): `Transform.transform` (one point or a list) and
  `transform_vector`, `BoundingBox.get_local_vertices`,
  `get_world_vertices` and `contains`, and `Rotation.get_normalized`
  (`Rotation(1e10).get_normalized().pitch` is `0.0`, as in LibCarla) follow
  LibCarla's float formulas on its own float32 matrix, and
  `Quaternion(rotation)` and `rotator` follow ue5-dev's `Quaternion.h`
  (`cosf`, `sinf`, `asinf`, `atan2f`, double degree conversions). CARLA
  0.10.0 has no `Quaternion`, so those two are checked against ue5-dev's
  header compiled into a small C++ program; the others against the official
  0.10.0 module. Double fields stay double, as in LibCarla: `GeoLocation`,
  the geo projections, `GeoEllipsoid`, `GeoOffsetTransform`,
  `OpendriveGenerationParameters` and `WorldSettings`' time steps
  (`fixed_delta_seconds`, `max_substep_delta_time`).
* **Float precision of the other value types (issue #90).** The scalar
  `float` fields of `VehicleControl`, `WalkerControl`,
  `VehicleAckermannControl`, `AckermannControllerSettings`,
  `VehiclePhysicsControl`, `WheelPhysicsControl`, `WeatherParameters`,
  `WorldSettings` (`max_culling_distance`, `tile_stream_distance`,
  `actor_active_distance`), `FloatColor`, `OpticalFlowPixel`, `LightState`
  (`intensity`), `LidarDetection` (`intensity`), `SemanticLidarDetection`
  (`cos_inc_angle`) and `RadarDetection` store float32 as well, rounded on
  construction and on assignment as the official module does:
  `VehicleControl(throttle=0.1).throttle` is `0.10000000149011612`. The
  lists `VehiclePhysicsControl.forward_gear_ratios` and
  `reverse_gear_ratios` are plain Codon lists, changed in place: their
  elements keep the doubles you put there and become float32 when sent to
  the server (the official 0.10.0 module cannot read them at all). Records
  only read from the server (measurements, telemetry, `Landmark`, ...)
  already hold LibCarla's float32 values.

* **`==` / `!=` (issue #70)** follow LibCarla's `operator==` on every value
  type the Python API compares. Float fields compare exactly *in float32*, as
  LibCarla stores them: `Location(0.1) == Location(0.1 + 1e-12)` is `True`
  (both store the same float32), as is `VehicleControl(throttle=0.1) ==
  VehicleControl(throttle=0.1 + 1e-12)`. Double fields (`GeoLocation`, the geo projections, `GeoEllipsoid`,
  `GeoOffsetTransform`, and `WorldSettings`' `fixed_delta_seconds` and
  `max_substep_delta_time`) compare exactly. As in
  LibCarla, some types compare less than every field. `Rotation`s are also
  equal when each pair of angles has `|a| + |b| == 180`, so
  `Rotation(90, 90, 90) == Rotation(-90, -90, -90)`. `Color` ignores alpha.
  `Timestamp` and `WorldSnapshot` compare the frame only. `ActorAttribute`
  compared with another attribute checks type and value, not the id.
  Compared with a `str` or `Color`, it reads itself as that type, raising
  `CarlaError` on a type mismatch (LibCarla's `BadAttributeCast`). Compared
  with a `float`, an `int` or a `bool`, it reads itself as a **float**, as the
  Python API does: Boost.Python's float overload accepts any Python int,
  bools included. So `attr == 4` on an int attribute and `attr == True` on a
  bool attribute raise in both, and `fov_attr == 90` is `True`. Write
  `attr.as_int() == 4`, `int(attr) == 4` (Python API only) or
  `attr.as_bool()` instead. `as_bool()` accepts "true"/"false" in any case
  and raises on anything else, as LibCarla. Comparing unrelated types does
  not compile; the Python API answers `False`.

* **Sensor data (issue #24).**
  - `raw_data` is a property, as in the Python API (issue #78), on `Image`,
    `LidarMeasurement`, `SemanticLidarMeasurement`, `RadarMeasurement`,
    `DVSEventArray`, `OpticalFlowImage` and `FakeImage`. It returns a
    `RawData`, a read-only zero-copy view standing in for the `memoryview`
    (`len`, indexing, slicing with any step, iteration, `in`, `==`, `hash`,
    `nbytes`, `readonly`, `tolist()`, `tobytes()`); writing through it is a
    compile error, and it keeps the measurement's buffer alive. `repr` /
    `str` give `<memory of N bytes>` where a `memoryview` gives
    `<memory at 0x...>`. For a `FakeImage`, `append` / `extend` move the
    bytes to a new buffer, so an earlier view keeps showing the bytes it was
    taken of (a Python `memoryview` would block the resize instead); `del`
    shifts them in place, and an earlier view sees the shifted bytes.
    `FakeImage(width, height, fov, data)` copies `data`: changing the
    caller's list afterwards does not change the image. Codon
    has no `bytes`: `bytes(m.raw_data)` becomes `m.raw_data.tobytes()` (a
    copy in a `str`), and, as Codon's `np.frombuffer`
    takes only a `str`, `np.frombuffer(m.raw_data, dtype=np.uint8)` becomes
    `np.frombuffer(m.raw_data.tobytes(), dtype=np.uint8)`. The zero-copy
    pointer is `raw_data_ptr()` (a `Ptr[u8]`, valid while the measurement is
    alive), with `raw_size()` in bytes. Reading an element (`image[i]`,
    `radar[i]`, ...) gives a copy; assigning one (`image[i] =
    carla.Color(...)`, issue #33) writes into LibCarla's buffer, as in the
    Python API, so `raw_data` views see it. A field that does not fit
    LibCarla's element type (a color component outside [0, 255], a DVS `x` beyond
    uint16, a negative `object_idx`) raises `CarlaError`, where the Python
    API raises `OverflowError` / `TypeError`. Negative indices count from the
    end (the Python API rejects them).
  - `Image.save_to_disk` writes PNG only, as LibCarla does when built with
    PNG support only (CARLA's default build): any other extension is replaced
    by `.png`, missing directories are created, and the path written is
    returned. Depth and LogarithmicDepth give 8-bit gray, the others 8-bit
    RGBA. The file is written uncompressed (stored deflate blocks, about 4
    bytes per pixel), because LibCarla's `ImageIO` needs libpng's headers,
    which LibCarla's build does not export to its users. The converters
    reproduce LibCarla's `ColorConverter` (with Boost.GIL's rounding) and use
    LibCarla's CityScapes palette (29 tags in 0.10.0, 30 in ue5-dev). One
    PNG IDAT chunk holds the image, which limits it to about 0.5 Gpx.
  - Paths of `save_to_disk` (images and point clouds) go through LibCarla's
    `FileSystem::ValidateFilePath`. A bare file name (`"out.png"`) raises
    `CarlaError` with CARLA 0.10.0 and ue5-dev alike, as it raises in the
    official API: its parent path is empty, which libstdc++'s
    `create_directories` (0.10.0) and `absolute` (ue5-dev) reject. Write
    `"./out.png"` or give a directory.
  - `LaneInvasionEvent.actor` always raises `CarlaError` ("trying to operate
    on a destroyed actor ..."), as it raises `RuntimeError` in the Python API:
    lane invasion is computed on the client, and LibCarla (0.10.0 and
    ue5-dev) never gives these events an episode to resolve the actor in. The
    actor is the lane-invasion sensor's parent.
  - `LaneInvasionSensor`, `ServerSideSensor` and `ClientSideSensor` are the
    same type as `Sensor` (issue #33). In the Python API they are subclasses.
  - `OpticalFlowImage.get_color_coded_flow()` returns a `FakeImage` (the
    Python API's name) with `raw_data`, `pixel(x, y)` and, as the Python
    API's byte vector, indexing, assignment, `del`, iteration, `in`, `append`
    and `extend` on single bytes (issue #33; slices are not supported).
  - DVS events are packed 13-byte records in LibCarla; `DVSEventArray` reads
    them in place, and `to_image()` / `to_array*()` are computed in Codon
    from that view (same results as LibCarla's `ToImage` / `ToArray*`).
  - **V2X depends on the LibCarla version** (issue #42). `Sensor.send`,
    `SensorData.as_cam_event()` / `as_custom_v2x_event()` and the
    `CAMEvent` / `CAMMessage` / `CustomV2XEvent` / `CustomV2XData` types
    need LibCarla ue5-dev, which has `ServerSideSensor::Send` and the V2X
    measurement types. Built against LibCarla 0.10.0, which has neither,
    they compile but raise `CarlaError` ("... is not available in LibCarla
    0.10.0"); no measurement is ever `SensorDataType.CAM` / `CustomV2X`.
    A CARLA 0.10.0 server has no `sensor.other.v2x*` blueprints at all.
    `CustomV2XBytes` is a plain value and works with both.
  - `CAMMessage.get()` returns a typed `CAM`, where the Python API returns a
    nested dict (the dict keys work too; see below). It holds the same content: the header (`ItsPduHeader`), the
    generation time, the station type, the reference position (with the
    altitude, which the Python API drops), and the containers of LibCarla's
    `LibITS.h` as `Optional`s: `basic_vehicle_container_high_frequency`
    (heading, speed, ... as `ValueConfidence`; its optional fields as
    `Optional`), `protected_communication_zones_rsu` (a road-side unit's
    `List[ProtectedCommunicationZone]`) and
    `basic_vehicle_container_low_frequency` (vehicle role, exterior lights,
    `List[PathPoint]`). Field names are LibITS's in snake_case and values
    are LibCarla's raw ETSI codes and units (latitude in 0.1 microdegrees,
    speed in cm/s, ...), where the Python API turns the enumerations into
    display strings ("Passenger Car", "CAM"). This is the whole CAM that
    LibCarla carries; there is no further ITS tree (DENM etc.) to bind.
    The power is `CAMMessage.power`, not part of `get()`. An empty path
    history is `[]` (the Python API gives `None`). As the Python API builds
    a new dict per call, `get()` returns a new snapshot each time: changing
    one result (or `power`) afterwards does not affect earlier or later
    `get()` results. `CustomV2XData.get()` likewise returns a `CustomV2XMessage`
    (`header`, `data`: `CustomV2XBytes`).
  - **The Python API's dict keys work on the typed results** (issue #85), so
    `event[0].get()["Message"]["Message"]["DataSize"]` (upstream
    `smoke/test_v2x.py`) ports unchanged. Each literal key is resolved at
    compile time and has its own static type, mirroring the dict of
    `PythonAPI/carla/src/V2XData.cpp` level by level: `get()` has `"Power"`
    and `"Message"`; `["Message"]` has `"Header"` (`"Protocol Version"`,
    `"Message ID"`, `"Station ID"`) and `"Message"`: for a custom message
    `"DataSize"`, `"MaxDataSize"`, `"Bytes"`, for a CAM `"Generation Delta
    Time"` and `"CAM Parameters"` with the Python API's containers and key
    names (its spelling `"Position Confidence Eliipse"` and `"Performance
    class"` included). As in the dict, enumerations are the display strings
    (`"CUSTOM"`, `"Passenger Car"`, `"Unavailable"`), a value the dict holds
    as `None` is an `Optional` (`"Acceleration Control"`, an empty `"Path
    History"`), and a key the dict leaves out (the container that is not
    present, an absent `"Expiry Time"`) raises `KeyError`. `"Bytes"` is a
    `str` of the raw bytes (Codon's `b"..."` literal is a `str` too, so
    `payload["Bytes"] == b"hello v2x"` compiles and holds). An unknown key
    (`"Altitude"`, which the dict lacks) is a compile error listing the
    dict's keys, and so is a computed (non-literal) key: use the typed
    attributes for those. Check a `None`-valued key with `is None` before
    indexing into it: `hf["Steering Wheel Angle"]["Value"]` compiles (Codon
    unwraps the `Optional` implicitly) but fails at run time when the value
    is absent. The levels are views of one `get()` snapshot (no copy). This
    is statically checked, so it is not a compatibility path and does not
    warn.
  - `CustomV2XBytes.get_bytes()` / `set_bytes()` use `List[int]` (Codon has
    no `bytes`); `set_bytes` rejects values outside [0, 255] with
    `CarlaError`. `data_size` can be set within [0, 100] only (the Python
    API accepts any uint8 and then reads past the 100-byte buffer).
    `CustomV2XBytes.get()` returns the object itself, which takes the
    dict's `"DataSize"` / `"MaxDataSize"` / `"Bytes"` keys.
  - `Sensor.send` also takes a `str` (sent as `set_string` would store it).
    As in LibCarla, sending through a server-side sensor that is not
    `sensor.other.v2x_custom` only logs a warning; the client-side
    lane-invasion sensor raises `ActorTypeError`.
  - The mock backend mirrors ue5-dev's V2X sensors with synthetic messages:
    a CAM from each `sensor.other.v2x` every tick (every `gen_cam_min` with
    `fixed_rate`), built from the parent vehicle's state (a sensor without a
    vehicle or walker parent is a road-side unit), and custom messages
    delivered at the next tick to the other `sensor.other.v2x_custom`
    sensors on the same `channel_id`. Receivers within `filter_distance` get
    everything at the transmit power (no path-loss model).
  - **G-buffer textures (issue #33).** `Sensor.listen_to_gbuffer(gbuffer_id,
    callback)` works like `listen(callback)`: the callback receives a
    `SensorData` (`as_image()`) at the dispatch points, on the program's
    thread. Only RGB cameras serve G-buffers: another sensor raises
    `CarlaError` (LibCarla logs a warning and delivers nothing). As in
    LibCarla, `is_listening` becomes `True`, and
    `stop()` leaves the G-buffer streams running; `stop_gbuffer(id)`,
    `destroy()` or a batch `DestroyActor` ends them (they are stopped before
    the actor is destroyed: LibCarla cannot unsubscribe a destroyed actor's
    G-buffer stream, which would then reconnect forever). An id outside
    `GBufferTextureID` raises `CarlaError` (LibCarla aborts the process).
    G-buffer streams need a server that serves them: a CARLA 0.10.0 server
    does not, and rejects the subscription, so `listen_to_gbuffer` raises
    `CarlaError` ("the server rejected the G-buffer subscription ...") and
    nothing is registered. With the official 0.10.0 Python API the same call
    aborts the whole process (an uncaught `std::exception` in LibCarla).
    **Do not call it against a 0.10.0 server:** the subscription is rejected
    only after the client timeout, and the server process then crashes and
    restarts, dropping every client's world. `get_server_version()` cannot
    tell such a server apart (ue5-dev servers also report "0.10.0"), so call
    it only against a server known to stream G-buffers. The integration test
    runs this check only with `TSC_TEST_GBUFFER=1`.
  - **ROS2 (issue #33).** `enable_for_ros()` / `disable_for_ros()` /
    `is_enabled_for_ros()` call LibCarla's `ServerSideSensor`; a server built
    without ROS2 accepts the requests and reports `False`.
  - The `ServerSideSensor` methods raise `ActorTypeError` on a lane-invasion
    sensor, which LibCarla computes on the client (in the Python API, a
    `carla.ClientSideSensor` has no such methods).

**Not a difference: lookups that can miss return `None`, as in Python.**
`World.get_actor`, `World.try_spawn_actor`, `ActorList.find`,
`WorldSnapshot.find`, `Map.get_waypoint`, `Waypoint.get_left_lane` /
`get_right_lane` / `get_junction`, `Vehicle.get_traffic_light` and
`World.get_random_location_from_navigation` return `None` in the same cases as
the official API: an unknown actor id, an occupied spawn point, no lane at
the location or beyond the outermost lane, no junction, no traffic light, no
navigation mesh. (`Sensor.poll`, which has no Python counterpart, returns
`None` when the queue is empty.) The tests check these cases against a real
server, except the navigation-mesh case, which only the mock exercises.
`get_actor(id)` of an actor destroyed in the same episode still returns an
`Actor`: LibCarla caches actors on the client and never evicts them, and the
official API does the same. Use the world snapshot to tell whether an actor
still exists, not `is_alive`: a real server can report `True` for such an
actor, while the mock backend reports `False`.

Python-style code works without changes in most cases: `if world.get_actor(id) is None`,
`if wp:`, and using the value directly when it is known to exist
(`world.try_spawn_actor(bp, t).destroy()`, `m.get_waypoint(loc).next(2.0)`).
The exception is a chained lookup passed into *annotated* code, which needs
an explicit `unwrap()` (see [Codon limitation 5](#codon-limitations-found-while-building-this)).
The return type is `Optional[T]`, which only adds static information: the
checker knows what the value is when it is not `None`, so
`actor.set_transform(m.get_waypoint(loc))` is a compile error. Two details
differ at run time:

* Using a `None` value raises `ValueError` (`optional unpack failed: expected
  Actor, got None`) where Python raises `AttributeError` (`'NoneType' object
  has no attribute ...`).
* For an id outside the uint32 range (e.g. `-1`), `get_actor` and the `find`
  lookups return `None`; the official API raises `OverflowError`.

`str()` gives the official text, field for field (issue #71):
`Transform(Location(x=1.000000, y=2.000000, z=3.000000), Rotation(...))`,
`Actor(id=24, type=vehicle.tesla.model3)` for every actor class,
`ActorAttribute(id=number_of_wheels,type=int,value=4(const))`, ... Floats
are printed as the binding does, with `std::to_string` (`%f`, after rounding
`float` fields to float32) or `std::ostream` (`%g`), by a formatter that
matches glibc exactly (`-0.000000`, `-nan`, half-to-even ties). `repr()` is
the same text, where Python gives `<carla.Location object at 0x...>`. Three
cases differ:

* `WorldSettings` with `fixed_delta_seconds` unset prints
  `fixed_delta_seconds=None`; the official `str()` raises
  `RuntimeError: bad_optional_access`.
* `CollisionEvent` and `ObstacleDetectionEvent` print
  `other_actor=Actor(id=.., type=..)`; the official binding streams the
  actor's shared pointer, i.e. a memory address.
* Classes the official module prints with Python's default repr
  (`SensorData`, `CAMEvent`, `CustomV2XEvent`, `Junction`, `Landmark`, ...)
  keep a descriptive typesafe_carla repr.

## Codon limitations found while building this

These affect how the design's guarantees should be read:

1. **Codon only type-checks functions that are called.** A function nobody
   calls is not checked, even with full annotations, so a downstream library
   is checked where an application uses it, not on its own. Every program in
   `tests/compile` calls its functions so the tests really exercise the
   checker. The programs are compiled, never run.
2. **Exceptions do not form a catchable hierarchy.** User exceptions must
   derive `Static[Exception]`, and `except CarlaError` does not catch
   `TimeoutError`. Catch the specific classes. Each one carries the native
   status in `.status`.
3. **`__del__` is not registered automatically in Codon 0.19.3.**
   `class_alloc` passes an untyped pointer to `register_finalizer`, so
   finalizers never run. `_ffi.Handle` registers its own, which is what
   releases native handles. `tests/unit/test_ownership.codon` checks this.
4. **`-D` defines take integer values only**, so the library path can't be
   baked in at compile time. It comes from `TYPESAFE_CARLA_LIB` at run time,
   with the RPATH as the fallback.
5. **`Optional[T]` is implicitly unwrapped.** Codon accepts an
   `Optional[Waypoint]` where a `Waypoint` is expected and raises at run
   time if it is `None`, so forgetting the `is None` check after
   `get_actor`, `get_left_lane` and similar calls is not a compile error.
   Codon does not narrow types after an `is not None` check either.
   Implicit unwrapping also has a gap: the result of an `Optional`-returning
   method called on an `Optional` value (`left = wp.get_left_lane()` where
   `wp = m.get_waypoint(loc)`) cannot go where a non-`Optional` type is
   *annotated* (a typed parameter, `x: Waypoint = ...`, a `-> Waypoint`
   return): Codon reports `'Waypoint' does not match expected type
   'Optional[Waypoint]'`, even after an `is not None` check. Unannotated
   code is not affected. Unwrap explicitly with Codon's `unwrap()`:
   `unwrap(wp).get_left_lane()` or `unwrap(wp.get_left_lane())`.
6. **Subclass values do not upcast inside `Optional`, `isinstance` is exact,
   and arguments typed late bind badly.** `Location` derives from `Vector3D`,
   but Codon rejects an `Optional[Location]` for an `Optional[Vector3D]`
   parameter, and `isinstance(location, Vector3D)` is `False`. An argument
   reached through an implicitly unwrapped `Optional` can be bound to a
   `Vector3D` parameter before Codon knows that it is a `Location`. The vector
   and location parameters of the API (and of `Vector3D`'s methods) are
   therefore generic and checked statically. This is also what lets a
   `Location` passed as a `Vector3D`, or a `Vector3D` passed as a `Location`
   (both implicit in the Python API), warn, or fail in strict mode. Other
   types still fail with `'Rotation' does not match expected type 'Vector3D'`.
7. **`-D` defines are visible only in the main file**, not in imported
   modules. The launcher passes the strict-mode setting to the library
   through a generated module (`_tsc_build_config`) instead.
8. Float format specifiers (`f"{x:.6f}"`) need an installed `en_US` locale in
   Codon 0.19.3, so `str()` formats floats with its own `%f` / `%g`
   implementation (`typesafe_carla/_fmt.codon`).
9. **Class hierarchies two levels deep are miscompiled.** With `class B(A)`
   and `class C(B)`, a method `C` inherits from `B` reads `A`'s fields at the
   wrong offset (a `C(3)` reports `x == 0` through `B`'s methods), and
   `super().__init__()` chains through both levels crash. The API keeps every
   class one level below `Actor`: `TrafficLight` and `TrafficSign` are
   siblings (see Differences).

## License

typesafe_carla is released under the [MIT License](LICENSE). LibCarla (MIT)
is linked into the native library and its license is shipped as
`_native/LICENSE.CARLA`. LibCarla brings in more third-party code, also
linked statically:

- Boost (BSL-1.0);
- rpclib (MIT), with its bundled asio, msgpack-c, cppformat and optional-lite;
- RecastNavigation (Zlib), libpng (libpng-2.0 AND Libpng) and zlib (Zlib);
- LibCarla's vendored pugixml, odrSpiral (Apache-2.0), moodycamel
  ConcurrentQueue, Fast-Quadric-Mesh-Simplification and MeshReconstruction;
- the other MIT copyright holders named in LibCarla's own sources (e.g. the
  DVS sensor, GNSS measurement and vehicle physics files).

Their notices are shipped as `_native/THIRD_PARTY_NOTICES`. The build collects
them from the fetched sources ([`cmake/ThirdPartyNotices.cmake`](cmake/ThirdPartyNotices.cmake)),
with each component's version, license and source path. Configuring fails if
a license text is missing, or if LibCarla links a library (directly or
transitively) or vendors third-party code the notices do not cover. Codon (Apache-2.0) is redistributed by `typesafe-carla-toolchain` with
its license.
