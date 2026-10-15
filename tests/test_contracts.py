# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile and execute current controller contracts without a console."""
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(os.environ.get("POORDS4_TEST_ROOT", ROOT)).resolve()
CHECKS = Path(__file__).resolve().parent / "checks"
PAYLOAD_CHECKS = (
    "input_api_guards", "lego_empty_read", "reader_snapshot",
    "reader_handle_cleanup", "source_device_discovery", "reader_route_recovery",
    "primary_neutral_route", "game_image_recovery", "device_class_observer",
    "device_class_import", "trigger_effect_import", "trigger_effect_controls",
    "trigger_effect_concurrency", "trigger_queued_transport",
    "reader_rumble_channel", "game_rumble_path", "recorded_client_routing",
)
ROOT_CHECKS = ("rc51_recovery", "rc51_trigger_path", "trigger_state_compat")


class ControllerContracts(unittest.TestCase):
    def run_check(self, name, arguments):
        env = os.environ.copy()
        env["POORDS4_HOST_CC"] = env.get("POORDS4_HOST_CC", "gcc")
        result = subprocess.run(
            [sys.executable, str(CHECKS / ("check_" + name + ".py")), *arguments],
            capture_output=True, text=True, env=env, timeout=90,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = json.loads(result.stdout)
        if "groups" in report:
            self.assertTrue(report["groups"])
            for group, row in report["groups"].items():
                self.assertTrue(row["compiled"] and row["passed"], group)
        else:
            self.assertTrue(report["compiled"] and report["passed"])


def payload_case(name, extra=()):
    def case(self):
        self.run_check(name, ["--payload", str(SOURCE / "payload"), *extra])
    return case


def root_case(name):
    def case(self):
        self.run_check(name, [str(SOURCE)])
    return case


for name in PAYLOAD_CHECKS:
    setattr(ControllerContracts, "test_" + name, payload_case(name))
for name in ROOT_CHECKS:
    setattr(ControllerContracts, "test_" + name, root_case(name))
ControllerContracts.test_connection_continuity = payload_case(
    "primary_neutral_route", ("--connection-continuity",)
)

if __name__ == "__main__":
    unittest.main(verbosity=2)
