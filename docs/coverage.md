# LibCarla API coverage

Which public methods of the main LibCarla client classes the C shim calls, for CARLA ref `0.10.0` (libcarla backend).

Regenerate with `uv run python -m tools.bindgen coverage --build-dir <build> -o docs/coverage.md`.

- **generated**: the binding is generated from `bindings/*.yaml`
- **hand-written**: called from a hand-written shim function
- **—**: not bound yet

**177 of 326 methods bound (54%), 44 of them generated.**

| class | bound |
|---|---|
| `carla::client::Client` | 19 / 27 |
| `carla::client::World` | 17 / 48 |
| `carla::client::Map` | 9 / 18 |
| `carla::client::Waypoint` | 13 / 22 |
| `carla::client::Junction` | 3 / 3 |
| `carla::client::Landmark` | 17 / 23 |
| `carla::client::Actor` | 22 / 37 |
| `carla::client::Vehicle` | 11 / 23 |
| `carla::client::Walker` | 3 / 8 |
| `carla::client::WalkerAIController` | 4 / 5 |
| `carla::client::TrafficLight` | 13 / 20 |
| `carla::client::Sensor` | 3 / 3 |
| `carla::client::BlueprintLibrary` | 4 / 10 |
| `carla::client::ActorBlueprint` | 6 / 11 |
| `carla::client::ActorList` | 3 / 8 |
| `carla::client::WorldSnapshot` | 5 / 10 |
| `carla::client::DebugHelper` | 5 / 5 |
| `carla::traffic_manager::TrafficManager` | 20 / 45 |

## Methods

### `carla::client::Client`

| method | binding |
|---|---|
| `ApplyBatch` | hand-written |
| `ApplyBatchSync` | hand-written |
| `GenerateOpenDriveWorld` | hand-written |
| `GetAvailableMaps` | — |
| `GetClientVersion` | hand-written |
| `GetCurrentEpisode` | — |
| `GetInstanceTM` | hand-written |
| `GetRequiredFiles` | — |
| `GetServerVersion` | hand-written |
| `GetTimeout` | hand-written |
| `GetWorld` | hand-written |
| `LoadWorld` | hand-written |
| `LoadWorldIfDifferent` | — |
| `ReloadWorld` | hand-written |
| `ReplayFile` | hand-written |
| `RequestFile` | — |
| `SetFilesBaseFolder` | — |
| `SetReplayerIgnoreHero` | — |
| `SetReplayerIgnoreSpectator` | — |
| `SetReplayerTimeFactor` | hand-written |
| `SetTimeout` | hand-written |
| `ShowRecorderActorsBlocked` | hand-written |
| `ShowRecorderCollisions` | hand-written |
| `ShowRecorderFileInfo` | hand-written |
| `StartRecorder` | hand-written |
| `StopRecorder` | hand-written |
| `StopReplayer` | hand-written |

### `carla::client::World`

| method | binding |
|---|---|
| `ApplyColorTextureToObject` | — |
| `ApplyColorTextureToObjects` | — |
| `ApplyFloatColorTextureToObject` | — |
| `ApplyFloatColorTextureToObjects` | — |
| `ApplySettings` | hand-written |
| `ApplyTexturesToObject` | — |
| `ApplyTexturesToObjects` | — |
| `CastRay` | — |
| `EnableEnvironmentObjects` | — |
| `FreezeAllTrafficLights` | — |
| `GetActor` | hand-written |
| `GetActors` | hand-written |
| `GetBlueprintLibrary` | hand-written |
| `GetEnvironmentObjects` | — |
| `GetEpisode` | — |
| `GetId` | hand-written |
| `GetLevelBBs` | — |
| `GetLightManager` | — |
| `GetMap` | hand-written |
| `GetNamesOfAllObjects` | — |
| `GetRandomLocationFromNavigation` | hand-written |
| `GetSettings` | hand-written |
| `GetSnapshot` | hand-written |
| `GetSpectator` | — |
| `GetTrafficLight` | — |
| `GetTrafficLightFromOpenDRIVE` | — |
| `GetTrafficLightsFromWaypoint` | — |
| `GetTrafficLightsInJunction` | — |
| `GetTrafficSign` | — |
| `GetVehiclesLightStates` | — |
| `GetWeather` | hand-written |
| `GroundProjection` | — |
| `IsWeatherEnabled` | hand-written |
| `LoadLevelLayer` | — |
| `MakeDebugHelper` | hand-written |
| `OnTick` | — |
| `ProjectPoint` | — |
| `RemoveOnTick` | — |
| `ResetAllTrafficLights` | — |
| `SetPedestriansCrossFactor` | — |
| `SetPedestriansSeed` | — |
| `SetWeather` | hand-written |
| `SpawnActor` | hand-written |
| `Tick` | hand-written |
| `TrySpawnActor` | hand-written |
| `UnloadLevelLayer` | — |
| `WaitForTick` | hand-written |
| `operator=` | — |

### `carla::client::Map`

| method | binding |
|---|---|
| `CalculateCrossedLanes` | — |
| `CookInMemoryMap` | — |
| `GenerateWaypoints` | hand-written |
| `GetAllCrosswalkZones` | hand-written |
| `GetAllLandmarks` | hand-written |
| `GetAllLandmarksOfType` | hand-written |
| `GetGeoReference` | — |
| `GetJunction` | — |
| `GetJunctionWaypoints` | — |
| `GetLandmarkGroup` | — |
| `GetLandmarksFromId` | — |
| `GetMap` | — |
| `GetName` | hand-written |
| `GetOpenDrive` | hand-written |
| `GetRecommendedSpawnPoints` | hand-written |
| `GetTopology` | hand-written |
| `GetWaypoint` | hand-written |
| `GetWaypointXODR` | — |

### `carla::client::Waypoint`

| method | binding |
|---|---|
| `GetAllLandmarksInDistance` | — |
| `GetDistance` | hand-written |
| `GetId` | hand-written |
| `GetJunction` | hand-written |
| `GetJunctionId` | hand-written |
| `GetLandmarksOfTypeInDistance` | — |
| `GetLaneChange` | — |
| `GetLaneId` | hand-written |
| `GetLaneWidth` | hand-written |
| `GetLeft` | hand-written |
| `GetLeftLaneMarking` | — |
| `GetNext` | — |
| `GetNextUntilLaneEnd` | — |
| `GetPrevious` | — |
| `GetPreviousUntilLaneStart` | — |
| `GetRight` | hand-written |
| `GetRightLaneMarking` | — |
| `GetRoadId` | hand-written |
| `GetSectionId` | hand-written |
| `GetTransform` | hand-written |
| `GetType` | hand-written |
| `IsJunction` | hand-written |

### `carla::client::Junction`

| method | binding |
|---|---|
| `GetBoundingBox` | hand-written |
| `GetId` | hand-written |
| `GetWaypoints` | hand-written |

### `carla::client::Landmark`

| method | binding |
|---|---|
| `GetCountry` | hand-written |
| `GetDistance` | hand-written |
| `GetHeight` | hand-written |
| `GetId` | hand-written |
| `GetName` | hand-written |
| `GetOrientation` | hand-written |
| `GetPitch` | — |
| `GetRoadId` | hand-written |
| `GetRoll` | — |
| `GetS` | hand-written |
| `GetSubType` | hand-written |
| `GetT` | hand-written |
| `GetText` | hand-written |
| `GetTransform` | hand-written |
| `GetType` | hand-written |
| `GetUnit` | hand-written |
| `GetValidities` | — |
| `GetValue` | hand-written |
| `GetWaypoint` | — |
| `GetWidth` | hand-written |
| `GetZOffset` | hand-written |
| `GethOffset` | — |
| `IsDynamic` | — |

### `carla::client::Actor`

| method | binding |
|---|---|
| `AddAngularImpulse` | generated |
| `AddForce` | generated |
| `AddImpulse` | generated |
| `AddTorque` | generated |
| `ApplyTexture` | — |
| `Destroy` | hand-written |
| `DisableConstantVelocity` | — |
| `EnableConstantVelocity` | — |
| `GetAcceleration` | generated |
| `GetActorClassName` | — |
| `GetActorName` | — |
| `GetActorState` | hand-written |
| `GetAngularVelocity` | generated |
| `GetAttributes` | — |
| `GetBoundingBox` | generated |
| `GetDisplayId` | — |
| `GetId` | generated |
| `GetLocation` | generated |
| `GetParent` | — |
| `GetParentId` | — |
| `GetSemanticTags` | — |
| `GetTransform` | generated |
| `GetTypeId` | generated |
| `GetVelocity` | generated |
| `GetWorld` | — |
| `IsActive` | — |
| `IsAlive` | generated |
| `IsDormant` | — |
| `Serialize` | hand-written |
| `SetActorDead` | — |
| `SetCollisions` | — |
| `SetEnableGravity` | generated |
| `SetLocation` | generated |
| `SetSimulatePhysics` | generated |
| `SetTargetAngularVelocity` | generated |
| `SetTargetVelocity` | generated |
| `SetTransform` | generated |

### `carla::client::Vehicle`

| method | binding |
|---|---|
| `ApplyAckermannControl` | — |
| `ApplyAckermannControllerSettings` | — |
| `ApplyControl` | hand-written |
| `ApplyPhysicsControl` | hand-written |
| `CloseDoor` | — |
| `EnableCarSim` | — |
| `EnableChronoPhysics` | — |
| `GetAckermannControllerSettings` | — |
| `GetControl` | hand-written |
| `GetFailureState` | — |
| `GetLightState` | generated |
| `GetPhysicsControl` | hand-written |
| `GetSpeedLimit` | generated |
| `GetTrafficLight` | hand-written |
| `GetTrafficLightState` | generated |
| `GetWheelSteerAngle` | — |
| `IsAtTrafficLight` | generated |
| `OpenDoor` | — |
| `SetAutopilot` | generated |
| `SetLightState` | generated |
| `SetWheelSteerDirection` | — |
| `ShowDebugTelemetry` | — |
| `UseCarSimRoad` | — |

### `carla::client::Walker`

| method | binding |
|---|---|
| `ApplyControl` | hand-written |
| `BlendPose` | hand-written |
| `GetBonesTransform` | — |
| `GetPoseFromAnimation` | — |
| `GetWalkerControl` | hand-written |
| `HidePose` | — |
| `SetBonesTransform` | — |
| `ShowPose` | — |

### `carla::client::WalkerAIController`

| method | binding |
|---|---|
| `GetRandomLocation` | — |
| `GoToLocation` | generated |
| `SetMaxSpeed` | generated |
| `Start` | generated |
| `Stop` | generated |

### `carla::client::TrafficLight`

| method | binding |
|---|---|
| `Freeze` | generated |
| `GetAffectedLaneWaypoints` | — |
| `GetElapsedTime` | hand-written |
| `GetGreenTime` | hand-written |
| `GetGroupTrafficLights` | — |
| `GetLightBoxes` | — |
| `GetOpenDRIVEID` | — |
| `GetPoleIndex` | hand-written |
| `GetRedTime` | hand-written |
| `GetSignId` | — |
| `GetState` | hand-written |
| `GetStopWaypoints` | — |
| `GetTriggerVolume` | — |
| `GetYellowTime` | hand-written |
| `IsFrozen` | hand-written |
| `ResetGroup` | generated |
| `SetGreenTime` | generated |
| `SetRedTime` | generated |
| `SetState` | generated |
| `SetYellowTime` | generated |

### `carla::client::Sensor`

| method | binding |
|---|---|
| `IsListening` | hand-written |
| `Listen` | hand-written |
| `Stop` | hand-written |

### `carla::client::BlueprintLibrary`

| method | binding |
|---|---|
| `Filter` | hand-written |
| `FilterByAttribute` | — |
| `Find` | hand-written |
| `at` | hand-written |
| `begin` | — |
| `empty` | — |
| `end` | — |
| `operator=` | — |
| `operator[]` | — |
| `size` | hand-written |

### `carla::client::ActorBlueprint`

| method | binding |
|---|---|
| `ContainsAttribute` | hand-written |
| `ContainsTag` | hand-written |
| `GetAttribute` | hand-written |
| `GetId` | hand-written |
| `GetTags` | — |
| `MakeActorDescription` | hand-written |
| `MatchTags` | — |
| `SetAttribute` | hand-written |
| `begin` | — |
| `end` | — |
| `size` | — |

### `carla::client::ActorList`

| method | binding |
|---|---|
| `Filter` | hand-written |
| `Find` | — |
| `at` | hand-written |
| `begin` | — |
| `empty` | — |
| `end` | — |
| `operator[]` | — |
| `size` | hand-written |

### `carla::client::WorldSnapshot`

| method | binding |
|---|---|
| `Contains` | — |
| `Find` | hand-written |
| `GetFrame` | — |
| `GetId` | hand-written |
| `GetTimestamp` | hand-written |
| `begin` | — |
| `end` | — |
| `operator!=` | — |
| `operator==` | hand-written |
| `size` | hand-written |

### `carla::client::DebugHelper`

| method | binding |
|---|---|
| `DrawArrow` | hand-written |
| `DrawBox` | hand-written |
| `DrawLine` | hand-written |
| `DrawPoint` | hand-written |
| `DrawString` | hand-written |

### `carla::traffic_manager::TrafficManager`

| method | binding |
|---|---|
| `GetActionBuffer` | — |
| `GetNextAction` | — |
| `IsValidPort` | — |
| `Port` | generated |
| `RegisterVehicles` | — |
| `Release` | — |
| `RemoveImportedRoute` | — |
| `RemoveUploadPath` | — |
| `Reset` | — |
| `SetAutoLaneChange` | generated |
| `SetBoundariesRespawnDormantVehicles` | — |
| `SetCollisionDetection` | — |
| `SetCustomPath` | — |
| `SetDesiredSpeed` | hand-written |
| `SetDistanceToLeadingVehicle` | hand-written |
| `SetForceLaneChange` | generated |
| `SetGlobalDistanceToLeadingVehicle` | generated |
| `SetGlobalLaneOffset` | — |
| `SetGlobalPercentageSpeedDifference` | generated |
| `SetHybridPhysicsMode` | generated |
| `SetHybridPhysicsRadius` | — |
| `SetImportedRoute` | — |
| `SetKeepRightPercentage` | hand-written |
| `SetLaneOffset` | hand-written |
| `SetMaxBoundaries` | — |
| `SetOSMMode` | — |
| `SetPercentageIgnoreVehicles` | hand-written |
| `SetPercentageIgnoreWalkers` | hand-written |
| `SetPercentageRunningLight` | hand-written |
| `SetPercentageRunningSign` | hand-written |
| `SetPercentageSpeedDifference` | hand-written |
| `SetRandomDeviceSeed` | generated |
| `SetRandomLeftLaneChangePercentage` | hand-written |
| `SetRandomRightLaneChangePercentage` | hand-written |
| `SetRespawnDormantVehicles` | — |
| `SetSynchronousMode` | generated |
| `SetSynchronousModeTimeOutInMiliSecond` | — |
| `SetUpdateVehicleLights` | generated |
| `ShutDown` | — |
| `SynchronousTick` | — |
| `Tick` | — |
| `UnregisterVehicles` | — |
| `UpdateImportedRoute` | — |
| `UpdateUploadPath` | — |
| `operator=` | — |
