#!/usr/bin/env python3
import pathlib
import subprocess
import tempfile

UPDATER = pathlib.Path(__file__).resolve().parents[1] / "deploy/capy-bearer-update"


def run(*args, cwd=None):
    return subprocess.run(args, cwd=cwd, text=True, capture_output=True, timeout=10)


with tempfile.TemporaryDirectory(prefix="capy-deploy-approval-") as directory:
    repo = pathlib.Path(directory)
    work = repo / "docs/work"
    work.mkdir(parents=True)
    assert run("git", "init", "-q", directory).returncode == 0
    assert run("git", "config", "user.name", "Approval gate test", cwd=repo).returncode == 0
    assert run("git", "config", "user.email", "gate@example.invalid", cwd=repo).returncode == 0
    record = work / "approval.md"

    def commit(text):
        record.write_text(text)
        assert run("git", "add", "--", "docs/work/approval.md", cwd=repo).returncode == 0
        staged = run("git", "diff", "--cached", "--name-only", cwd=repo)
        assert staged.stdout.strip() == "docs/work/approval.md", staged.stdout
        print(run("git", "status", "--short", cwd=repo).stdout, end="")
        result = run("git", "-c", "core.hooksPath=/dev/null", "commit", "-qm", "Update approval fixture", cwd=repo)
        assert result.returncode == 0, result.stderr
        return run("git", "rev-parse", "HEAD", cwd=repo).stdout.strip()

    def check(revision):
        return run("bash", str(UPDATER), "--check-approval", str(repo), revision)

    ordinary = commit("# Ordinary tasks\n- [ ] Finish a routine task.\n")
    result = check(ordinary)
    assert result.returncode == 0, result.stderr
    assert "Approval scan passed" in result.stdout
    print("PASS: ordinary unchecked task and no marker")

    held = commit("# Approval\n- [ ] DEPLOY_APPROVAL: Await the human decision.\n")
    result = check(held)
    assert result.returncode == 1, result
    assert f"{held}:docs/work/approval.md:2:" in result.stdout, result.stdout
    assert "Approval held:" in result.stdout
    print(result.stdout, end="")

    record.write_text("# Approval\n- [x] DEPLOY_APPROVAL: Test-only release.\n")
    assert check(held).returncode == 1
    print("PASS: an uncommitted release does not clear a committed hold")

    released = commit(record.read_text())
    result = check(released)
    assert result.returncode == 0, result.stderr
    assert f"{released}:docs/work/approval.md:2:" in result.stdout, result.stdout
    assert "Approval released:" in result.stdout
    print(result.stdout, end="")
    assert check(held).returncode == 1
    print("PASS: the old held revision remains blocked after release")

    result = check("missing-revision")
    assert result.returncode != 0
    assert "Approval scan failed" in result.stderr
    print("PASS: a revision lookup error fails closed")

print("Deployment approval gate tests passed")
