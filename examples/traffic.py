# Populate the town: autopilot vehicles under the Traffic Manager and walkers
# with AI controllers, in synchronous mode. Then remove everything.
import typesafe_carla as carla

client = carla.Client("localhost", 2000)
client.set_timeout(20.0)
world = client.get_world()
lib = world.get_blueprint_library()
tm = client.get_trafficmanager(8000)

original = world.get_settings()
settings = world.get_settings()
settings.synchronous_mode = True
settings.fixed_delta_seconds = 0.05
world.apply_settings(settings)
tm.set_synchronous_mode(True)
tm.global_percentage_speed_difference(10.0)

spawned = List[int]()
try:
    vehicle_bp = lib.filter("vehicle.*")[0]
    batch = [carla.command.SpawnActor(vehicle_bp, p).then(
                 carla.command.SetAutopilot(carla.command.FutureActor, True, tm.get_port()))
             for p in world.get_map().get_spawn_points()[:10]]
    for r in client.apply_batch_sync(batch, True):
        if not r.has_error():
            spawned.append(r.actor_id)

    walker_bp = lib.filter("walker.pedestrian.*")[0]
    controllers = List[carla.WalkerAIController]()
    for _ in range(5):
        nav = world.get_random_location_from_navigation()
        if nav is None:
            continue
        actor = world.try_spawn_actor(walker_bp, carla.Transform(carla.Location(nav.x, nav.y, nav.z + 1.0)))
        if actor is None:
            continue
        walker = actor.as_walker()
        ai = world.spawn_actor(lib.find("controller.ai.walker"), carla.Transform(),
                               attach_to=walker).as_walker_ai_controller()
        spawned.append(walker.id)
        spawned.append(ai.id)
        controllers.append(ai)
    world.tick()
    for ai in controllers:
        ai.start()
        target = world.get_random_location_from_navigation()
        if target is not None:
            ai.go_to_location(target)

    for step in range(200):
        frame = world.tick()
        if step % 50 == 0:
            snapshot = world.get_snapshot()
            moving = 0
            for i in spawned:
                s = snapshot.find(i)
                if s is not None and s.get_velocity().length() > 0.5:
                    moving += 1
            print("frame", frame, "moving actors", moving, "of", len(spawned))
    for ai in controllers:
        ai.stop()
finally:
    tm.set_synchronous_mode(False)
    world.apply_settings(original)
    client.apply_batch([carla.command.DestroyActor(i) for i in reversed(spawned)])
