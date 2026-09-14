#!/usr/bin/env python3
import importlib.util
import os
import json
import pathlib
import shutil
import subprocess
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("arty_release_pilot", HERE / "arty_release_pilot.py")
pilot = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(pilot)
HELPER_PATH = pathlib.Path(os.environ.get("ARTY_RELEASE_HELPER", "/root/scripts/arty/release.py"))
HELPER_SPEC = importlib.util.spec_from_file_location("arty_release_helper", HELPER_PATH)
helper = importlib.util.module_from_spec(HELPER_SPEC)
HELPER_SPEC.loader.exec_module(helper)


class ArtyReleasePilotTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)
        self.root.chmod(0o755)
        self.source = self.root / "source"
        self.source.mkdir()
        self.write_source("one")
        self.write_contract()
        self.git("init")
        self.git("config", "user.email", "test@example.invalid")
        self.git("config", "user.name", "Capy test")
        self.git("add", ".")
        self.git("commit", "-m", "initial")

    def tearDown(self):
        self.temp.cleanup()

    def git(self, *args):
        return subprocess.run(["git", "-C", str(self.source), *args], check=True, capture_output=True, text=True, timeout=30)

    def write_source(self, value):
        for relative, executable in pilot.PAYLOAD:
            path = self.source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(value + relative)
            path.chmod(executable)

    def write_contract(self):
        lockfile = self.source / "editors/vscode/package-lock.json"
        lockfile.parent.mkdir(parents=True, exist_ok=True)
        lockfile.write_text("{}\n")
        contract = json.loads((HERE.parent / "deploy/arty.json").read_text())
        path = self.source / "deploy/arty.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(contract))

    def external_receipt(self):
        receipt = self.root / "acceptance.json"
        receipt.write_text(json.dumps({
            "schema_version": 1,
            "source_revision": self.git("rev-parse", "HEAD").stdout.strip(),
            "artifacts": [
                {"path": relative, "sha256": pilot.sha256(self.source / relative)}
                for relative, _ in pilot.PAYLOAD
            ],
        }, sort_keys=True) + "\n")
        return receipt

    def external_test_receipt(self):
        receipt = self.root / "test-receipt.json"
        receipt.write_text(json.dumps({
            "schema_version": 1,
            "source_revision": self.git("rev-parse", "HEAD").stdout.strip(),
            "tests": [{"command": "scripts/run_cli_tests.sh", "result": "passed"}],
        }, sort_keys=True) + "\n")
        return receipt

    def stage(self, name, receipt=None):
        target = self.root / name
        pilot.command_stage(type("Args", (), {
            "source": self.source, "stage": target, "revision": None,
            "acceptance_receipt": receipt or self.external_receipt(),
            "test_receipt": self.external_test_receipt(),
        })())
        return target

    def test_receipts_accept_additional_fields(self):
        receipt = self.external_receipt()
        data = json.loads(receipt.read_text())
        data["build_platform"] = "linux"
        data["artifacts"][0]["bytes"] = 1
        receipt.write_text(json.dumps(data))
        tests = self.external_test_receipt()
        data = json.loads(tests.read_text())
        data["limitations"] = ["none"]
        data["tests"][0]["seconds"] = 1.5
        tests.write_text(json.dumps(data))
        revision = self.git("rev-parse", "HEAD").stdout.strip()
        self.assertEqual(pilot.read_receipt(receipt)[0], revision)
        self.assertEqual(pilot.read_test_receipt(tests), revision)

    def test_stage_copies_external_receipt_and_rejects_unidentified_outputs(self):
        receipt = self.external_receipt()
        bundle = self.stage("bundle", receipt)
        self.assertEqual((bundle / pilot.RECEIPT).read_bytes(), receipt.read_bytes())
        for relative, _ in pilot.PAYLOAD:
            self.assertEqual(pilot.sha256(bundle / relative), pilot.sha256(self.source / relative))
        receipt_data = json.loads(receipt.read_text())
        receipt_data["artifacts"][0]["sha256"] = "0" * 64
        receipt.write_text(json.dumps(receipt_data))
        with self.assertRaisesRegex(ValueError, "does not match the source artifact"):
            self.stage("rejected", receipt)
        self.assertFalse((self.root / "rejected").exists())

    def test_manifest_uses_real_helper_and_fetched_contract_checks_exact_set(self):
        bundle = self.stage("bundle")
        output = self.root / "manifest.json"
        pilot.command_manifest(type("Args", (), {
            "source": self.source, "stage": bundle, "output": output,
            "release_helper": HELPER_PATH, "timeout": 30,
        })())
        manifest = json.loads(output.read_text())
        self.assertEqual(manifest["provenance"]["test_receipt"]["path"], pilot.TEST_RECEIPT)
        fetched = self.root / "fetched"
        for artifact in manifest["artifacts"]:
            destination = fetched / artifact["path"]
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(bundle / artifact["path"], destination)
        helper.verify_fetched_artifacts(fetched, manifest)
        self.assertEqual((fetched / pilot.TEST_RECEIPT).read_bytes(), (bundle / pilot.TEST_RECEIPT).read_bytes())
        (fetched / "extra").write_text("not in the contract")
        with self.assertRaisesRegex(helper.ReleaseError, "artifact set"):
            helper.verify_fetched_artifacts(fetched, manifest)

    def test_receipts_require_the_supported_schema(self):
        for receipt, reader in [(self.external_receipt(), pilot.read_receipt), (self.external_test_receipt(), pilot.read_test_receipt)]:
            value = json.loads(receipt.read_text())
            value["schema_version"] = 2
            receipt.write_text(json.dumps(value))
            with self.assertRaisesRegex(ValueError, "invalid shape"):
                reader(receipt)

    def test_manifest_rejects_a_stage_that_no_longer_matches_receipt(self):
        bundle = self.stage("bundle")
        (bundle / pilot.PAYLOAD[0][0]).write_text("changed")
        with self.assertRaisesRegex(ValueError, "does not match the acceptance receipt"):
            pilot.command_manifest(type("Args", (), {
                "source": self.source, "stage": bundle, "output": self.root / "manifest.json",
                "release_helper": HELPER_PATH, "timeout": 30,
            })())


if __name__ == "__main__":
    unittest.main(verbosity=2)
