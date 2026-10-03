# LibCarla API coverage

Which public methods of the main LibCarla client classes the C shim calls, for CARLA ref `0.10.0` (libcarla backend).

Regenerate with `uv run python -m tools.bindgen coverage --build-dir <build> -o docs/coverage.md`.

- **generated**: the binding is generated from `bindings/*.yaml`
- **hand-written**: called from a hand-written shim function
- **—**: not bound yet

**314 of 378 methods bound (83%), 219 of them generated.**

| class | bound |
|---|---|
| `carla::client::Client` | 26 / 27 |
| `carla::client::World` | 44 / 48 |
| `carla::client::Map` | 15 / 18 |
| `carla::client::Waypoint` | 22 / 22 |
| `carla::client::Junction` | 3 / 3 |
| `carla::client::Landmark` | 23 / 23 |
| `carla::client::Actor` | 33 / 37 |
| `carla::client::Vehicle` | 23 / 23 |
| `carla::client::Walker` | 8 / 8 |
| `carla::client::WalkerAIController` | 4 / 5 |
| `carla::client::TrafficSign` | 1 / 2 |
| `carla::client::TrafficLight` | 18 / 18 |
| `carla::client::Sensor` | 3 / 3 |
| `carla::client::BlueprintLibrary` | 4 / 10 |
| `carla::client::ActorBlueprint` | 9 / 11 |
| `carla::client::ActorList` | 2 / 8 |
| `carla::client::WorldSnapshot` | 5 / 10 |
| `carla::client::DebugHelper` | 5 / 5 |
| `carla::client::LightManager` | 10 / 18 |
| `carla::traffic_manager::TrafficManager` | 31 / 45 |
| `carla::sensor::SensorData` | 3 / 3 |
| `carla::sensor::data::LidarMeasurement` | 2 / 3 |
| `carla::sensor::data::SemanticLidarMeasurement` | 2 / 3 |
| `carla::sensor::data::RadarMeasurement` | 1 / 1 |
| `carla::sensor::data::GnssMeasurement` | 3 / 4 |
| `carla::sensor::data::IMUMeasurement` | 3 / 3 |
| `carla::sensor::data::CollisionEvent` | 3 / 3 |
| `carla::sensor::data::ObstacleDetectionEvent` | 3 / 3 |
| `carla::sensor::data::LaneInvasionEvent` | 2 / 2 |
| `carla::sensor::data::DVSEventArray` | 3 / 9 |

## Methods

### `carla::client::Client`

| method | binding |
|---|---|
| `ApplyBatch` | hand-written |
| `ApplyBatchSync` | hand-written |
| `GenerateOpenDriveWorld` | generated |
| `GetAvailableMaps` | generated |
| `GetClientVersion` | generated |
| `GetCurrentEpisode` | — |
| `GetInstanceTM` | generated |
| `GetRequiredFiles` | generated |
| `GetServerVersion` | generated |
| `GetTimeout` | generated |
| `GetWorld` | generated |
| `LoadWorld` | generated |
| `LoadWorldIfDifferent` | hand-written |
| `ReloadWorld` | generated |
| `ReplayFile` | generated |
| `RequestFile` | generated |
| `SetFilesBaseFolder` | generated |
| `SetReplayerIgnoreHero` | generated |
| `SetReplayerIgnoreSpectator` | generated |
| `SetReplayerTimeFactor` | generated |
| `SetTimeout` | generated |
| `ShowRecorderActorsBlocked` | generated |
| `ShowRecorderCollisions` | generated |
| `ShowRecorderFileInfo` | generated |
| `StartRecorder` | generated |
| `StopRecorder` | generated |
| `StopReplayer` | generated |

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
| `GetActors` | generated |
| `GetBlueprintLibrary` | generated |
| `GetEnvironmentObjects` | generated |
| `GetEpisode` | hand-written |
| `GetId` | generated |
| `GetLevelBBs` | generated |
| `GetLightManager` | generated |
| `GetMap` | generated |
| `GetNamesOfAllObjects` | generated |
| `GetRandomLocationFromNavigation` | generated |
| `GetSettings` | hand-written |
| `GetSnapshot` | generated |
| `GetSpectator` | generated |
| `GetTrafficLight` | generated |
| `GetTrafficLightFromOpenDRIVE` | generated |
| `GetTrafficLightsFromWaypoint` | generated |
| `GetTrafficLightsInJunction` | generated |
| `GetTrafficSign` | generated |
| `GetVehiclesLightStates` | generated |
| `GetWeather` | generated |
| `GroundProjection` | generated |
| `IsWeatherEnabled` | generated |
| `LoadLevelLayer` | generated |
| `MakeDebugHelper` | hand-written |
| `OnTick` | hand-written |
| `ProjectPoint` | generated |
| `RemoveOnTick` | hand-written |
| `ResetAllTrafficLights` | generated |
| `SetPedestriansCrossFactor` | generated |
| `SetPedestriansSeed` | generated |
| `SetWeather` | generated |
| `SpawnActor` | generated |
| `Tick` | generated |
| `TrySpawnActor` | generated |
| `UnloadLevelLayer` | generated |
| `WaitForTick` | generated |
| `operator=` | — |

### `carla::client::Map`

| method | binding |
|---|---|
| `CalculateCrossedLanes` | — |
| `CookInMemoryMap` | generated |
| `GenerateWaypoints` | generated |
| `GetAllCrosswalkZones` | generated |
| `GetAllLandmarks` | generated |
| `GetAllLandmarksOfType` | generated |
| `GetGeoReference` | generated |
| `GetJunction` | — |
| `GetJunctionWaypoints` | — |
| `GetLandmarkGroup` | generated |
| `GetLandmarksFromId` | generated |
| `GetMap` | hand-written |
| `GetName` | generated |
| `GetOpenDrive` | generated |
| `GetRecommendedSpawnPoints` | generated |
| `GetTopology` | generated |
| `GetWaypoint` | generated |
| `GetWaypointXODR` | generated |

### `carla::client::Waypoint`

| method | binding |
|---|---|
| `GetAllLandmarksInDistance` | generated |
| `GetDistance` | hand-written |
| `GetId` | hand-written |
| `GetJunction` | generated |
| `GetJunctionId` | hand-written |
| `GetLandmarksOfTypeInDistance` | generated |
| `GetLaneChange` | generated |
| `GetLaneId` | hand-written |
| `GetLaneWidth` | hand-written |
| `GetLeft` | generated |
| `GetLeftLaneMarking` | generated |
| `GetNext` | generated |
| `GetNextUntilLaneEnd` | generated |
| `GetPrevious` | generated |
| `GetPreviousUntilLaneStart` | generated |
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
| `GetBoundingBox` | generated |
| `GetId` | generated |
| `GetWaypoints` | generated |

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
| `Destroy` | generated |
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
| `GetWorld` | generated |
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
| `ApplyControl` | generated |
| `ApplyPhysicsControl` | hand-written |
| `CloseDoor` | generated |
| `EnableCarSim` | generated |
| `EnableChronoPhysics` | generated |
| `GetAckermannControllerSettings` | generated |
| `GetControl` | generated |
| `GetFailureState` | generated |
| `GetLightState` | generated |
| `GetPhysicsControl` | generated |
| `GetSpeedLimit` | generated |
| `GetTrafficLight` | generated |
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
| `ApplyControl` | generated |
| `BlendPose` | generated |
| `GetBonesTransform` | generated |
| `GetPoseFromAnimation` | generated |
| `GetWalkerControl` | generated |
| `HidePose` | generated |
| `SetBonesTransform` | generated |
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
| `IsListening` | generated |
| `Listen` | hand-written |
| `Stop` | hand-written |

### `carla::client::BlueprintLibrary`

| method | binding |
|---|---|
| `Filter` | generated |
| `FilterByAttribute` | generated |
| `Find` | hand-written |
| `at` | — |
| `begin` | — |
| `empty` | — |
| `end` | — |
| `operator=` | — |
| `operator[]` | — |
| `size` | hand-written |

### `carla::client::ActorBlueprint`

| method | binding |
|---|---|
| `ContainsAttribute` | generated |
| `ContainsTag` | generated |
| `GetAttribute` | hand-written |
| `GetId` | generated |
| `GetTags` | generated |
| `MakeActorDescription` | hand-written |
| `MatchTags` | generated |
| `SetAttribute` | hand-written |
| `begin` | — |
| `end` | — |
| `size` | generated |

### `carla::client::ActorList`

| method | binding |
|---|---|
| `Filter` | generated |
| `Find` | — |
| `at` | — |
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
| `GetId` | generated |
| `GetTimestamp` | generated |
| `begin` | — |
| `end` | — |
| `operator!=` | — |
| `operator==` | — |
| `size` | hand-written |

### `carla::client::DebugHelper`

| method | binding |
|---|---|
| `DrawArrow` | generated |
| `DrawBox` | generated |
| `DrawLine` | generated |
| `DrawPoint` | generated |
| `DrawString` | generated |

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

### `carla::sensor::SensorData`

| method | binding |
|---|---|
| `GetFrame` | hand-written |
| `GetSensorTransform` | hand-written |
| `GetTimestamp` | hand-written |

### `carla::sensor::data::LidarMeasurement`

| method | binding |
|---|---|
| `GetChannelCount` | hand-written |
| `GetHorizontalAngle` | hand-written |
| `GetPointCount` | — |

### `carla::sensor::data::SemanticLidarMeasurement`

| method | binding |
|---|---|
| `GetChannelCount` | hand-written |
| `GetHorizontalAngle` | hand-written |
| `GetPointCount` | — |

### `carla::sensor::data::RadarMeasurement`

| method | binding |
|---|---|
| `GetDetectionAmount` | hand-written |

### `carla::sensor::data::GnssMeasurement`

| method | binding |
|---|---|
| `GetAltitude` | hand-written |
| `GetGeoLocation` | — |
| `GetLatitude` | hand-written |
| `GetLongitude` | hand-written |

### `carla::sensor::data::IMUMeasurement`

| method | binding |
|---|---|
| `GetAccelerometer` | hand-written |
| `GetCompass` | hand-written |
| `GetGyroscope` | hand-written |

### `carla::sensor::data::CollisionEvent`

| method | binding |
|---|---|
| `GetActor` | generated |
| `GetNormalImpulse` | hand-written |
| `GetOtherActor` | generated |

### `carla::sensor::data::ObstacleDetectionEvent`

| method | binding |
|---|---|
| `GetActor` | generated |
| `GetDistance` | generated |
| `GetOtherActor` | generated |

### `carla::sensor::data::LaneInvasionEvent`

| method | binding |
|---|---|
| `GetActor` | generated |
| `GetCrossedLaneMarkings` | generated |

### `carla::sensor::data::DVSEventArray`

| method | binding |
|---|---|
| `GetFOVAngle` | hand-written |
| `GetHeight` | hand-written |
| `GetWidth` | hand-written |
| `ToArray` | — |
| `ToArrayPol` | — |
| `ToArrayT` | — |
| `ToArrayX` | — |
| `ToArrayY` | — |
| `ToImage` | — |
