# LibCarla API coverage

Which public methods of the main LibCarla client classes the C shim calls, for CARLA ref `0.10.0` (libcarla backend).

Regenerate with `uv run python -m tools.bindgen coverage --build-dir <build> -o docs/coverage.md`.

- **generated**: the binding is generated from `bindings/*.yaml`
- **hand-written**: called from a hand-written shim function
- **—**: not bound yet

**285 of 344 methods bound (82%), 140 of them generated.**

| class | bound |
|---|---|
| `carla::client::Client` | 26 / 27 |
| `carla::client::World` | 44 / 48 |
| `carla::client::Map` | 15 / 18 |
| `carla::client::Waypoint` | 18 / 22 |
| `carla::client::Junction` | 3 / 3 |
| `carla::client::Landmark` | 23 / 23 |
| `carla::client::Actor` | 32 / 37 |
| `carla::client::Vehicle` | 23 / 23 |
| `carla::client::Walker` | 8 / 8 |
| `carla::client::WalkerAIController` | 4 / 5 |
| `carla::client::TrafficSign` | 1 / 2 |
| `carla::client::TrafficLight` | 18 / 18 |
| `carla::client::Sensor` | 3 / 3 |
| `carla::client::BlueprintLibrary` | 5 / 10 |
| `carla::client::ActorBlueprint` | 8 / 11 |
| `carla::client::ActorList` | 3 / 8 |
| `carla::client::WorldSnapshot` | 5 / 10 |
| `carla::client::DebugHelper` | 5 / 5 |
| `carla::client::LightManager` | 10 / 18 |
| `carla::traffic_manager::TrafficManager` | 31 / 45 |

## Methods

### `carla::client::Client`

| method | binding |
|---|---|
| `ApplyBatch` | hand-written |
| `ApplyBatchSync` | hand-written |
| `GenerateOpenDriveWorld` | hand-written |
| `GetAvailableMaps` | generated |
| `GetClientVersion` | hand-written |
| `GetCurrentEpisode` | — |
| `GetInstanceTM` | hand-written |
| `GetRequiredFiles` | generated |
| `GetServerVersion` | hand-written |
| `GetTimeout` | hand-written |
| `GetWorld` | hand-written |
| `LoadWorld` | hand-written |
| `LoadWorldIfDifferent` | hand-written |
| `ReloadWorld` | hand-written |
| `ReplayFile` | hand-written |
| `RequestFile` | generated |
| `SetFilesBaseFolder` | generated |
| `SetReplayerIgnoreHero` | generated |
| `SetReplayerIgnoreSpectator` | generated |
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
| `ApplyColorTextureToObjects` | generated |
| `ApplyFloatColorTextureToObject` | — |
| `ApplyFloatColorTextureToObjects` | generated |
| `ApplySettings` | hand-written |
| `ApplyTexturesToObject` | — |
| `ApplyTexturesToObjects` | generated |
| `CastRay` | generated |
| `EnableEnvironmentObjects` | generated |
| `FreezeAllTrafficLights` | generated |
| `GetActor` | hand-written |
| `GetActors` | hand-written |
| `GetBlueprintLibrary` | hand-written |
| `GetEnvironmentObjects` | generated |
| `GetEpisode` | hand-written |
| `GetId` | hand-written |
| `GetLevelBBs` | generated |
| `GetLightManager` | generated |
| `GetMap` | hand-written |
| `GetNamesOfAllObjects` | generated |
| `GetRandomLocationFromNavigation` | hand-written |
| `GetSettings` | hand-written |
| `GetSnapshot` | hand-written |
| `GetSpectator` | generated |
| `GetTrafficLight` | generated |
| `GetTrafficLightFromOpenDRIVE` | generated |
| `GetTrafficLightsFromWaypoint` | generated |
| `GetTrafficLightsInJunction` | generated |
| `GetTrafficSign` | generated |
| `GetVehiclesLightStates` | generated |
| `GetWeather` | hand-written |
| `GroundProjection` | generated |
| `IsWeatherEnabled` | hand-written |
| `LoadLevelLayer` | generated |
| `MakeDebugHelper` | hand-written |
| `OnTick` | hand-written |
| `ProjectPoint` | generated |
| `RemoveOnTick` | hand-written |
| `ResetAllTrafficLights` | generated |
| `SetPedestriansCrossFactor` | generated |
| `SetPedestriansSeed` | generated |
| `SetWeather` | hand-written |
| `SpawnActor` | hand-written |
| `Tick` | hand-written |
| `TrySpawnActor` | hand-written |
| `UnloadLevelLayer` | generated |
| `WaitForTick` | hand-written |
| `operator=` | — |

### `carla::client::Map`

| method | binding |
|---|---|
| `CalculateCrossedLanes` | — |
| `CookInMemoryMap` | generated |
| `GenerateWaypoints` | hand-written |
| `GetAllCrosswalkZones` | hand-written |
| `GetAllLandmarks` | hand-written |
| `GetAllLandmarksOfType` | hand-written |
| `GetGeoReference` | generated |
| `GetJunction` | — |
| `GetJunctionWaypoints` | — |
| `GetLandmarkGroup` | generated |
| `GetLandmarksFromId` | generated |
| `GetMap` | hand-written |
| `GetName` | hand-written |
| `GetOpenDrive` | hand-written |
| `GetRecommendedSpawnPoints` | hand-written |
| `GetTopology` | hand-written |
| `GetWaypoint` | hand-written |
| `GetWaypointXODR` | generated |

### `carla::client::Waypoint`

| method | binding |
|---|---|
| `GetAllLandmarksInDistance` | generated |
| `GetDistance` | hand-written |
| `GetId` | hand-written |
| `GetJunction` | hand-written |
| `GetJunctionId` | hand-written |
| `GetLandmarksOfTypeInDistance` | generated |
| `GetLaneChange` | generated |
| `GetLaneId` | hand-written |
| `GetLaneWidth` | hand-written |
| `GetLeft` | generated |
| `GetLeftLaneMarking` | generated |
| `GetNext` | — |
| `GetNextUntilLaneEnd` | — |
| `GetPrevious` | — |
| `GetPreviousUntilLaneStart` | — |
| `GetRight` | generated |
| `GetRightLaneMarking` | generated |
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
| `GetPitch` | generated |
| `GetRoadId` | hand-written |
| `GetRoll` | generated |
| `GetS` | hand-written |
| `GetSubType` | hand-written |
| `GetT` | hand-written |
| `GetText` | hand-written |
| `GetTransform` | hand-written |
| `GetType` | hand-written |
| `GetUnit` | hand-written |
| `GetValidities` | generated |
| `GetValue` | hand-written |
| `GetWaypoint` | generated |
| `GetWidth` | hand-written |
| `GetZOffset` | hand-written |
| `GethOffset` | generated |
| `IsDynamic` | generated |

### `carla::client::Actor`

| method | binding |
|---|---|
| `AddAngularImpulse` | generated |
| `AddForce` | generated |
| `AddImpulse` | generated |
| `AddTorque` | generated |
| `ApplyTexture` | generated |
| `Destroy` | hand-written |
| `DisableConstantVelocity` | generated |
| `EnableConstantVelocity` | generated |
| `GetAcceleration` | generated |
| `GetActorClassName` | generated |
| `GetActorName` | generated |
| `GetActorState` | generated |
| `GetAngularVelocity` | generated |
| `GetAttributes` | hand-written |
| `GetBoundingBox` | generated |
| `GetDisplayId` | — |
| `GetId` | generated |
| `GetLocation` | generated |
| `GetParent` | generated |
| `GetParentId` | — |
| `GetSemanticTags` | generated |
| `GetTransform` | generated |
| `GetTypeId` | generated |
| `GetVelocity` | generated |
| `GetWorld` | — |
| `IsActive` | generated |
| `IsAlive` | generated |
| `IsDormant` | generated |
| `Serialize` | — |
| `SetActorDead` | — |
| `SetCollisions` | generated |
| `SetEnableGravity` | generated |
| `SetLocation` | generated |
| `SetSimulatePhysics` | generated |
| `SetTargetAngularVelocity` | generated |
| `SetTargetVelocity` | generated |
| `SetTransform` | generated |

### `carla::client::Vehicle`

| method | binding |
|---|---|
| `ApplyAckermannControl` | generated |
| `ApplyAckermannControllerSettings` | generated |
| `ApplyControl` | hand-written |
| `ApplyPhysicsControl` | hand-written |
| `CloseDoor` | generated |
| `EnableCarSim` | generated |
| `EnableChronoPhysics` | generated |
| `GetAckermannControllerSettings` | generated |
| `GetControl` | hand-written |
| `GetFailureState` | generated |
| `GetLightState` | generated |
| `GetPhysicsControl` | hand-written |
| `GetSpeedLimit` | generated |
| `GetTrafficLight` | hand-written |
| `GetTrafficLightState` | generated |
| `GetWheelSteerAngle` | generated |
| `IsAtTrafficLight` | generated |
| `OpenDoor` | generated |
| `SetAutopilot` | generated |
| `SetLightState` | generated |
| `SetWheelSteerDirection` | generated |
| `ShowDebugTelemetry` | generated |
| `UseCarSimRoad` | generated |

### `carla::client::Walker`

| method | binding |
|---|---|
| `ApplyControl` | hand-written |
| `BlendPose` | generated |
| `GetBonesTransform` | hand-written |
| `GetPoseFromAnimation` | generated |
| `GetWalkerControl` | hand-written |
| `HidePose` | generated |
| `SetBonesTransform` | hand-written |
| `ShowPose` | generated |

### `carla::client::WalkerAIController`

| method | binding |
|---|---|
| `GetRandomLocation` | — |
| `GoToLocation` | generated |
| `SetMaxSpeed` | generated |
| `Start` | generated |
| `Stop` | generated |

### `carla::client::TrafficSign`

| method | binding |
|---|---|
| `GetSignId` | — |
| `GetTriggerVolume` | generated |

### `carla::client::TrafficLight`

| method | binding |
|---|---|
| `Freeze` | generated |
| `GetAffectedLaneWaypoints` | generated |
| `GetElapsedTime` | hand-written |
| `GetGreenTime` | hand-written |
| `GetGroupTrafficLights` | generated |
| `GetLightBoxes` | generated |
| `GetOpenDRIVEID` | generated |
| `GetPoleIndex` | hand-written |
| `GetRedTime` | hand-written |
| `GetState` | hand-written |
| `GetStopWaypoints` | generated |
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
| `FilterByAttribute` | hand-written |
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
| `GetTags` | generated |
| `MakeActorDescription` | hand-written |
| `MatchTags` | generated |
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
| `GetFrame` | hand-written |
| `GetId` | hand-written |
| `GetTimestamp` | hand-written |
| `begin` | — |
| `end` | — |
| `operator!=` | — |
| `operator==` | — |
| `size` | hand-written |

### `carla::client::DebugHelper`

| method | binding |
|---|---|
| `DrawArrow` | hand-written |
| `DrawBox` | hand-written |
| `DrawLine` | hand-written |
| `DrawPoint` | hand-written |
| `DrawString` | hand-written |

### `carla::client::LightManager`

| method | binding |
|---|---|
| `GetAllLights` | generated |
| `GetColor` | — |
| `GetIntensity` | — |
| `GetLightGroup` | — |
| `GetLightState` | hand-written |
| `GetTurnedOffLights` | generated |
| `GetTurnedOnLights` | generated |
| `IsActive` | — |
| `SetActive` | hand-written |
| `SetColor` | hand-written |
| `SetDayNightCycle` | generated |
| `SetEpisode` | — |
| `SetIntensity` | hand-written |
| `SetLightGroup` | hand-written |
| `SetLightState` | hand-written |
| `SetLightStateNoLock` | — |
| `TurnOff` | — |
| `TurnOn` | — |

### `carla::traffic_manager::TrafficManager`

| method | binding |
|---|---|
| `GetActionBuffer` | hand-written |
| `GetNextAction` | hand-written |
| `IsValidPort` | — |
| `Port` | generated |
| `RegisterVehicles` | — |
| `Release` | — |
| `RemoveImportedRoute` | — |
| `RemoveUploadPath` | — |
| `Reset` | — |
| `SetAutoLaneChange` | generated |
| `SetBoundariesRespawnDormantVehicles` | generated |
| `SetCollisionDetection` | generated |
| `SetCustomPath` | generated |
| `SetDesiredSpeed` | hand-written |
| `SetDistanceToLeadingVehicle` | hand-written |
| `SetForceLaneChange` | generated |
| `SetGlobalDistanceToLeadingVehicle` | generated |
| `SetGlobalLaneOffset` | generated |
| `SetGlobalPercentageSpeedDifference` | generated |
| `SetHybridPhysicsMode` | generated |
| `SetHybridPhysicsRadius` | generated |
| `SetImportedRoute` | generated |
| `SetKeepRightPercentage` | hand-written |
| `SetLaneOffset` | hand-written |
| `SetMaxBoundaries` | — |
| `SetOSMMode` | generated |
| `SetPercentageIgnoreVehicles` | hand-written |
| `SetPercentageIgnoreWalkers` | hand-written |
| `SetPercentageRunningLight` | hand-written |
| `SetPercentageRunningSign` | hand-written |
| `SetPercentageSpeedDifference` | hand-written |
| `SetRandomDeviceSeed` | generated |
| `SetRandomLeftLaneChangePercentage` | hand-written |
| `SetRandomRightLaneChangePercentage` | hand-written |
| `SetRespawnDormantVehicles` | generated |
| `SetSynchronousMode` | generated |
| `SetSynchronousModeTimeOutInMiliSecond` | — |
| `SetUpdateVehicleLights` | generated |
| `ShutDown` | generated |
| `SynchronousTick` | — |
| `Tick` | — |
| `UnregisterVehicles` | — |
| `UpdateImportedRoute` | — |
| `UpdateUploadPath` | — |
| `operator=` | — |
