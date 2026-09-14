#!/usr/bin/env python3
"""Stage an accepted Capy-Bearer release for the shared Arty helper."""

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
PAYLOAD = (
    ("bin/bearer_fastcgi.linux.bin", 0o755),
    ("bin/capyc", 0o755),
    ("bin/wasm/core.wasm", 0o644),
    ("scripts/bearer-cli", 0o755),
)
RECEIPT = "acceptance.json"
TEST_RECEIPT = "test-receipt.json"
SHARED_HELPER = pathlib.Path("/root/scripts/arty/release.py")
HEX = "0123456789abcdef"


def fail(message):
    raise ValueError(message)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def clean_revision(root):
    try:
        status = subprocess.run(
            ["git", "-C", str(root), "status", "--porcelain", "--untracked-files=all"],
            check=True, text=True, capture_output=True, timeout=30,
        )
        revision = subprocess.run(
            ["git", "-C", str(root), "rev-parse", "--verify", "HEAD^{commit}"],
            check=True, text=True, capture_output=True, timeout=30,
        ).stdout.strip()
    except (OSError, subprocess.SubprocessError) as error:
        fail(f"cannot inspect the source revision: {error}")
    if status.stdout:
        fail("the source worktree is not clean")
    if len(revision) != 40 or any(char not in HEX for char in revision):
        fail("Git returned an invalid source revision")
    return revision


def regular_file(root, relative):
    path = root / relative
    if not path.is_file() or path.is_symlink():
        fail(f"missing regular file: {relative}")
    return path


def read_receipt(path):
    try:
        value = json.loads(regular_file(path.parent, path.name).read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(f"invalid acceptance receipt: {error}")
    if not isinstance(value, dict) or not {"schema_version", "source_revision", "artifacts"} <= set(value) or value["schema_version"] != 1:
        fail("the acceptance receipt has an invalid shape")
    revision = value["source_revision"]
    if not isinstance(revision, str) or len(revision) != 40 or any(char not in HEX for char in revision):
        fail("the acceptance receipt has an invalid source revision")
    artifacts = value["artifacts"]
    if not isinstance(artifacts, list) or len(artifacts) != len(PAYLOAD):
        fail("the acceptance receipt has an invalid artifact list")
    recorded = {}
    for item in artifacts:
        if not isinstance(item, dict) or not {"path", "sha256"} <= set(item):
            fail("the acceptance receipt has an invalid artifact")
        relative, digest = item["path"], item["sha256"]
        if not isinstance(relative, str) or not isinstance(digest, str) or len(digest) != 64 or any(char not in HEX for char in digest):
            fail("the acceptance receipt has an invalid artifact")
        if relative in recorded:
            fail("the acceptance receipt repeats an artifact")
        recorded[relative] = digest
    if set(recorded) != {relative for relative, _ in PAYLOAD}:
        fail("the acceptance receipt does not name the full payload")
    return revision, recorded


def read_test_receipt(path):
    try:
        value = json.loads(regular_file(path.parent, path.name).read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(f"invalid test receipt: {error}")
    if not isinstance(value, dict) or not {"schema_version", "source_revision", "tests"} <= set(value) or value["schema_version"] != 1:
        fail("the test receipt has an invalid shape")
    revision, tests = value["source_revision"], value["tests"]
    if not isinstance(revision, str) or len(revision) != 40 or any(char not in HEX for char in revision):
        fail("the test receipt has an invalid source revision")
    if not isinstance(tests, list) or not tests:
        fail("the test receipt has no test evidence")
    for test in tests:
        if not isinstance(test, dict) or not {"command", "result"} <= set(test) or not isinstance(test["command"], str) or not test["command"] or test["result"] != "passed":
            fail("the test receipt has invalid test evidence")
    return revision


def validate_receipt(bundle):
    revision, recorded = read_receipt(bundle / RECEIPT)
    for relative, _ in PAYLOAD:
        if recorded[relative] != sha256(regular_file(bundle, relative)):
            fail(f"the payload digest does not match the acceptance receipt: {relative}")
    return revision


def command_stage(args):
    root = args.source.resolve()
    revision = clean_revision(root)
    if args.revision and args.revision != revision:
        fail("the requested revision does not match HEAD")
    acceptance = regular_file(args.acceptance_receipt.parent, args.acceptance_receipt.name)
    test_receipt = regular_file(args.test_receipt.parent, args.test_receipt.name)
    receipt_revision, recorded = read_receipt(acceptance)
    if receipt_revision != revision or read_test_receipt(test_receipt) != revision:
        fail("an external receipt does not match the clean source revision")
    for relative, _ in PAYLOAD:
        if recorded[relative] != sha256(regular_file(root, relative)):
            fail(f"the acceptance receipt does not match the source artifact: {relative}")
    stage = args.stage
    if stage.exists() or stage.is_symlink():
        fail(f"the stage already exists: {stage}")
    stage.parent.mkdir(parents=True, exist_ok=True)
    temporary = pathlib.Path(tempfile.mkdtemp(prefix=".capy-arty-", dir=stage.parent))
    try:
        for relative, mode in PAYLOAD:
            destination = temporary / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(regular_file(root, relative), destination)
            os.chmod(destination, mode)
        shutil.copyfile(acceptance, temporary / RECEIPT)
        shutil.copyfile(test_receipt, temporary / TEST_RECEIPT)
        os.chmod(temporary / RECEIPT, 0o644)
        os.chmod(temporary / TEST_RECEIPT, 0o644)
        validate_receipt(temporary)
        os.replace(temporary, stage)
    except BaseException:
        shutil.rmtree(temporary, ignore_errors=True)
        raise


def command_manifest(args):
    revision = validate_receipt(args.stage)
    if read_test_receipt(args.stage / TEST_RECEIPT) != revision:
        fail("the staged test receipt does not match the acceptance receipt")
    if clean_revision(args.source.resolve()) != revision:
        fail("the stage acceptance receipt does not match the clean source revision")
    try:
        subprocess.run(
            [sys.executable, str(args.release_helper), "manifest", "deploy/arty.json", "--stage", str(args.stage), "--output", str(args.output)],
            cwd=args.source, check=True, timeout=args.timeout,
        )
    except (OSError, subprocess.SubprocessError) as error:
        fail(f"the shared release helper failed: {error}")
    if clean_revision(args.source.resolve()) != revision:
        args.output.unlink(missing_ok=True)
        fail("the source revision changed while the manifest was created")


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(required=True)
    stage = subcommands.add_parser("stage")
    stage.add_argument("--stage", type=pathlib.Path, required=True)
    stage.add_argument("--acceptance-receipt", type=pathlib.Path, required=True)
    stage.add_argument("--test-receipt", type=pathlib.Path, required=True)
    stage.add_argument("--source", type=pathlib.Path, default=ROOT)
    stage.add_argument("--revision")
    stage.set_defaults(command=command_stage)
    manifest = subcommands.add_parser("manifest")
    manifest.add_argument("--stage", type=pathlib.Path, required=True)
    manifest.add_argument("--output", type=pathlib.Path, required=True)
    manifest.add_argument("--source", type=pathlib.Path, default=ROOT)
    manifest.add_argument("--release-helper", type=pathlib.Path, default=SHARED_HELPER)
    manifest.add_argument("--timeout", type=int, default=60)
    manifest.set_defaults(command=command_manifest)
    return parser.parse_args()


def main():
    args = parse_args()
    try:
        if hasattr(args, "timeout") and not 1 <= args.timeout <= 7200:
            fail("timeout must be between 1 and 7200 seconds")
        args.command(args)
    except (OSError, subprocess.SubprocessError, ValueError) as error:
        print(f"arty-release-pilot: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
