# Must not compile: design section 44, "vehicle.apply_control(Transform())
# fails before the executable is produced".
import typesafe_carla as carla

def drive(vehicle: carla.Vehicle):
    vehicle.apply_control(carla.Transform())

drive(carla.Client("localhost", 2000).get_world().get_actors()[0].as_vehicle())
