# Spawn a fleet of autopilot vehicles in one batch, watch them, remove them.
import typesafe_carla as carla

client = carla.Client("localhost", 2000)
client.set_timeout(20.0)
world = client.get_world()
points = world.get_map().get_spawn_points()
bp = world.get_blueprint_library().filter("vehicle.*")[0]

batch = [carla.command.SpawnActor(bp, p).then(
             carla.command.SetAutopilot(carla.command.FutureActor, True))
         for p in points[:4]]
ids = [r.actor_id for r in client.apply_batch_sync(batch, True) if not r.has_error()]
print("spawned", ids)
for _ in range(20):
    snapshot = world.wait_for_tick(10.0)
for i in ids:
    s = snapshot.find(i)
    if s is not None:
        print(i, s.get_transform().location)
client.apply_batch([carla.command.DestroyActor(i) for i in ids])
