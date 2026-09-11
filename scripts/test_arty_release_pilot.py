#!/usr/bin/env python3
import importlib.util
import json
import pathlib
import subprocess
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("arty_release_pilot", HERE / "arty_release_pilot.py")
pilot = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(pilot)


class ArtyReleasePilotTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)
        self.root.chmod(0o755)
        self.source = self.root / "source"
        self.source.mkdir()
        self.write_source("one")
        self.git("init")
        self.git("config", "user.email", "test@example.invalid")
        self.git("config", "user.name", "Capy test")
        self.git("add", ".")
        self.git("commit", "-m", "initial")
        self.releases = self.root / "installed" / "releases"
        self.releases.mkdir(parents=True)
        self.current = self.releases.parent / "current"

    def tearDown(self):
        self.temp.cleanup()

    def git(self, *args):
        return subprocess.run(["git", "-C", str(self.source), *args], check=True, capture_output=True, text=True)

    def write_source(self, value):
        for relative, executable in pilot.PAYLOAD:
            path = self.source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(value + relative)
            path.chmod(executable)
        abi = self.source / pilot.ABI_SOURCE
        abi.parent.mkdir(parents=True, exist_ok=True)
        abi.write_text(value + " ABI")

    def stage(self, name):
        target = self.root / name
        pilot.command_stage(type("Args", (), {"source": self.source, "stage": target, "revision": None})())
        return target

    def install(self, bundle):
        pilot.command_install(type("Args", (), {
            "bundle": bundle, "releases": self.releases, "current": self.current, "user": "root",
        })())

    def test_stage_has_complete_payload_and_strict_source_identity(self):
        bundle = self.stage("bundle")
        receipt = json.loads((bundle / pilot.RECEIPT).read_text())
        self.assertEqual(receipt["source_revision"], self.git("rev-parse", "HEAD").stdout.strip())
        self.assertEqual({item["path"] for item in receipt["artifacts"]}, {path for path, _ in pilot.PAYLOAD})
        for relative, _ in pilot.PAYLOAD:
            self.assertTrue((bundle / relative).is_file())
        self.assertEqual((bundle / pilot.RECEIPT).read_bytes(), (bundle / pilot.TEST_RECEIPT).read_bytes())
        (self.source / "uncommitted").write_text("no")
        with self.assertRaisesRegex(ValueError, "not clean"):
            self.stage("dirty")
        self.assertFalse((self.root / "dirty").exists())

    def test_install_failure_does_not_mutate_current_or_releases(self):
        bundle = self.stage("bundle")
        receipt = json.loads((bundle / pilot.RECEIPT).read_text())
        receipt["artifacts"][0]["sha256"] = "0" * 64
        (bundle / pilot.RECEIPT).write_text(json.dumps(receipt))
        with self.assertRaisesRegex(ValueError, "digest"):
            self.install(bundle)
        self.assertEqual(list(self.releases.iterdir()), [])
        self.assertFalse(self.current.exists() or self.current.is_symlink())

    def test_access_preflight_fails_without_mutating_release_state(self):
        bundle = self.stage("bundle")
        with self.assertRaises(subprocess.CalledProcessError):
            pilot.command_install(type("Args", (), {
                "bundle": bundle, "releases": self.releases, "current": self.current,
                "user": "capy-arty-no-such-user",
            })())
        self.assertEqual(list(self.releases.iterdir()), [])
        self.assertFalse(self.current.exists() or self.current.is_symlink())

    def test_stale_current_link_does_not_create_a_release(self):
        bundle = self.stage("bundle")
        revision = json.loads((bundle / pilot.RECEIPT).read_text())["source_revision"]
        stale = self.current.with_name(f".{self.current.name}.{revision}")
        stale.symlink_to("stale")
        with self.assertRaisesRegex(ValueError, "temporary current link"):
            self.install(bundle)
        self.assertEqual(list(self.releases.iterdir()), [])
        self.assertFalse(self.current.exists() or self.current.is_symlink())

    def test_install_preflights_an_unprivileged_user(self):
        bundle = self.stage("bundle")
        pilot.command_install(type("Args", (), {
            "bundle": bundle, "releases": self.releases, "current": self.current, "user": "nobody",
        })())
        self.assertTrue((self.current / "bin/capyc").is_file())

    def test_offline_local_rollback_keeps_previous_bundle(self):
        first = self.stage("first")
        first_revision = json.loads((first / pilot.RECEIPT).read_text())["source_revision"]
        self.install(first)
        self.write_source("two")
        self.git("add", ".")
        self.git("commit", "-m", "second")
        second = self.stage("second")
        second_revision = json.loads((second / pilot.RECEIPT).read_text())["source_revision"]
        self.install(second)
        self.assertEqual(self.current.resolve(), self.releases / second_revision)
        pilot.command_rollback(type("Args", (), {
            "releases": self.releases, "current": self.current, "revision": first_revision,
        })())
        self.assertEqual(self.current.resolve(), self.releases / first_revision)
        self.assertTrue((self.releases / second_revision).is_dir())


if __name__ == "__main__":
    unittest.main(verbosity=2)
