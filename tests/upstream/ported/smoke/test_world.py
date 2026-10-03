# Ported from CARLA's PythonAPI/test/smoke/test_world.py at carla-simulator/carla@0a5ce0d5b4952bd8294a163c12d49f197bdb2aba
# by tools/port_upstream_tests.py (typesafe_carla issue #80): the original needs a map
# (Town03 / Town05 / Town01) that no ue5-dev package ships. Do not edit by hand:
# change the rules there and regenerate. Changes:
#   - none: copied so that `from . import SmokeTest` uses the ported base, whose tearDown loads a shipped map instead of Town03
#
# Copyright (c) 2026 Computer Vision Center (CVC) at the Universitat Autonoma de
# Barcelona (UAB).
#
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT>.


from . import SmokeTest


class TestWorld(SmokeTest):
    def test_fixed_delta_seconds(self):
        print("TestWorld.test_fixed_delta_seconds")
        world = self.client.get_world()
        settings = world.get_settings()
        self.assertFalse(settings.synchronous_mode)
        for expected_delta_seconds in [0.1, 0.066667, 0.05, 0.033333, 0.016667, 0.011112]:
            settings.fixed_delta_seconds = expected_delta_seconds
            world.apply_settings(settings)
            for _ in range(0, 20):
                delta_seconds = world.wait_for_tick().timestamp.delta_seconds
                self.assertAlmostEqual(expected_delta_seconds, delta_seconds)
        settings.fixed_delta_seconds = None
        world.apply_settings(settings)
