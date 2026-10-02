# Typed control contract: a controller only accepts Vehicle + VehicleControl.
import typesafe_carla as carla

def proportional_steer(heading_error_deg: float) -> carla.VehicleControl:
    steer = max(-1.0, min(1.0, heading_error_deg / 45.0))
    return carla.VehicleControl(throttle=0.4, steer=steer)

def control(vehicle: carla.Vehicle, command: carla.VehicleControl):
    vehicle.apply_control(command)

client = carla.Client("localhost", 2000)
world = client.get_world()
for actor in world.get_actors().filter("vehicle.*"):
    vehicle = actor.as_vehicle()
    target_yaw = 30.0
    error = target_yaw - vehicle.get_transform().rotation.yaw
    control(vehicle, proportional_steer(error))
    print(vehicle, "->", vehicle.get_control())
