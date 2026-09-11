#!/usr/bin/env python3
"""Stage and install the Capy-Bearer Arty release-bundle pilot."""

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
ABI_SOURCE = "src/wasm/abi.h"
RECEIPT = "acceptance.json"
TEST_RECEIPT = "release-test.json"
SHARED_HELPER = pathlib.Path("/root/scripts/arty/release.py")


def fail(message):
    raise ValueError(message)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def clean_revision(root):
    status = subprocess.run(
        ["git", "-C", str(root), "status", "--porcelain"],
        check=True, text=True, capture_output=True,
    )
    if status.stdout:
        fail("the source worktree is not clean")
    return subprocess.run(
        ["git", "-C", str(root), "rev-parse", "HEAD"],
        check=True, text=True, capture_output=True,
    ).stdout.strip()


def regular_file(root, relative):
    path = root / relative
    if not path.is_file() or path.is_symlink():
        fail(f"missing regular file: {relative}")
    return path


def write_json(path, value):
    path.write_text(json.dumps(value, sort_keys=True, indent=2) + "\n")


def command_stage(args):
    root = args.source.resolve()
    revision = clean_revision(root)
    if args.revision and args.revision != revision:
        fail("the requested revision does not match HEAD")
    abi = regular_file(root, ABI_SOURCE)
    stage = args.stage
    if stage.exists():
        fail(f"the stage already exists: {stage}")
    stage.parent.mkdir(parents=True, exist_ok=True)
    temporary = pathlib.Path(tempfile.mkdtemp(prefix=".capy-arty-", dir=stage.parent))
    try:
        payload = []
        for relative, mode in PAYLOAD:
            source = regular_file(root, relative)
            destination = temporary / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
            os.chmod(destination, mode)
            payload.append({"path": relative, "sha256": sha256(source)})
        receipt = {
            "pilot": True,
            "source_revision": revision,
            "source_abi_sha256": sha256(abi),
            "artifacts": payload,
        }
        write_json(temporary / RECEIPT, receipt)
        write_json(temporary / TEST_RECEIPT, receipt)
        os.chmod(temporary / RECEIPT, 0o644)
        os.chmod(temporary / TEST_RECEIPT, 0o644)
        os.replace(temporary, stage)
    except BaseException:
        shutil.rmtree(temporary, ignore_errors=True)
        raise


def read_receipt(bundle):
    receipt = regular_file(bundle, RECEIPT)
    try:
        value = json.loads(receipt.read_text())
    except (OSError, json.JSONDecodeError) as error:
        fail(f"invalid acceptance receipt: {error}")
    expected = {"pilot", "source_revision", "source_abi_sha256", "artifacts"}
    if set(value) != expected or value["pilot"] is not True:
        fail("the acceptance receipt has an invalid shape")
    revision = value["source_revision"]
    if not isinstance(revision, str) or len(revision) != 40 or any(char not in "0123456789abcdef" for char in revision):
        fail("the acceptance receipt has an invalid source revision")
    if not isinstance(value["source_abi_sha256"], str) or len(value["source_abi_sha256"]) != 64:
        fail("the acceptance receipt has an invalid ABI identity")
    artifacts = value["artifacts"]
    if not isinstance(artifacts, list) or len(artifacts) != len(PAYLOAD):
        fail("the acceptance receipt has an invalid artifact list")
    recorded = {item.get("path"): item.get("sha256") for item in artifacts if isinstance(item, dict)}
    if set(recorded) != {path for path, _ in PAYLOAD}:
        fail("the acceptance receipt does not name the full payload")
    for relative, _ in PAYLOAD:
        if recorded[relative] != sha256(regular_file(bundle, relative)):
            fail(f"the payload digest does not match the receipt: {relative}")
    return revision


def test_access(user, path, flag):
    subprocess.run(["runuser", "-u", user, "--", "test", flag, str(path)], check=True)


def command_manifest(args):
    revision = read_receipt(args.stage)
    if clean_revision(args.source.resolve()) != revision:
        fail("the stage receipt does not match the clean source revision")
    try:
        subprocess.run([
            sys.executable, str(args.release_helper), "manifest", "deploy/arty.json", "--stage", str(args.stage),
            "--output", str(args.output),
        ], cwd=args.source, check=True)
    except subprocess.CalledProcessError as error:
        fail(f"the shared release helper failed: {error}")
    if clean_revision(args.source.resolve()) != revision:
        args.output.unlink(missing_ok=True)
        fail("the source revision changed while the manifest was created")


def command_install(args):
    bundle = args.bundle.resolve()
    revision = read_receipt(bundle)
    releases = args.releases.resolve()
    if not releases.is_dir() or releases.is_symlink():
        fail("the releases directory must already exist and must not be a symlink")
    target = releases / revision
    if target.exists() or target.is_symlink():
        fail(f"the release already exists: {target}")
    current = args.current
    if current.parent.resolve() != releases.parent:
        fail("the current link must be beside the releases directory")
    temporary = pathlib.Path(tempfile.mkdtemp(prefix=".capy-release-", dir=releases))
    try:
        os.chmod(temporary, 0o755)
        shutil.copytree(bundle, temporary / "bundle", symlinks=False)
        target_temp = temporary / "bundle"
        for relative, mode in PAYLOAD:
            os.chmod(target_temp / relative, mode)
        for directory in (target_temp, target_temp / "bin", target_temp / "bin/wasm", target_temp / "scripts"):
            os.chmod(directory, 0o755)
        for relative, mode in PAYLOAD:
            test_access(args.user, target_temp / relative, "-r")
            if mode & 0o111:
                test_access(args.user, target_temp / relative, "-x")
        link_temp = current.with_name(f".{current.name}.{revision}")
        if link_temp.exists() or link_temp.is_symlink():
            fail(f"temporary current link exists: {link_temp}")
        os.replace(target_temp, target)
        link_temp.symlink_to(pathlib.Path("releases") / revision)
        os.replace(link_temp, current)
    finally:
        shutil.rmtree(temporary, ignore_errors=True)


def command_rollback(args):
    releases = args.releases.resolve()
    target = releases / args.revision
    if not target.is_dir() or target.is_symlink():
        fail("the retained release does not exist")
    read_receipt(target)
    current = args.current
    if current.parent.resolve() != releases.parent:
        fail("the current link must be beside the releases directory")
    link_temp = current.with_name(f".{current.name}.{args.revision}")
    if link_temp.exists() or link_temp.is_symlink():
        fail(f"temporary current link exists: {link_temp}")
    link_temp.symlink_to(pathlib.Path("releases") / args.revision)
    os.replace(link_temp, current)


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(required=True)
    stage = subcommands.add_parser("stage")
    stage.add_argument("--stage", type=pathlib.Path, required=True)
    stage.add_argument("--source", type=pathlib.Path, default=ROOT)
    stage.add_argument("--revision")
    stage.set_defaults(command=command_stage)
    manifest = subcommands.add_parser("manifest")
    manifest.add_argument("--stage", type=pathlib.Path, required=True)
    manifest.add_argument("--output", type=pathlib.Path, required=True)
    manifest.add_argument("--source", type=pathlib.Path, default=ROOT)
    manifest.add_argument("--release-helper", type=pathlib.Path, default=SHARED_HELPER)
    manifest.set_defaults(command=command_manifest)
    install = subcommands.add_parser("install")
    install.add_argument("--bundle", type=pathlib.Path, required=True)
    install.add_argument("--releases", type=pathlib.Path, required=True)
    install.add_argument("--current", type=pathlib.Path, required=True)
    install.add_argument("--user", required=True)
    install.set_defaults(command=command_install)
    rollback = subcommands.add_parser("rollback")
    rollback.add_argument("--releases", type=pathlib.Path, required=True)
    rollback.add_argument("--current", type=pathlib.Path, required=True)
    rollback.add_argument("--revision", required=True)
    rollback.set_defaults(command=command_rollback)
    return parser.parse_args()


def main():
    args = parse_args()
    try:
        args.command(args)
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        print(f"arty-release-pilot: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
