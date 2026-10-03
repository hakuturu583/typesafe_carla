# Ported from CARLA's PythonAPI/test/smoke/test_client.py at carla-simulator/carla@0a5ce0d5b4952bd8294a163c12d49f197bdb2aba
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


class TestClient(SmokeTest):
    def test_version(self):
        print("TestClient.test_version")
        self.assertEqual(self.client.get_client_version(), self.client.get_server_version())
