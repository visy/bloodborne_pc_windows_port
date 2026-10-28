"""deploy.sh decisions (rollback target, dry run, no restart on a failure before the restart) and the
running_commit file the bot writes. Needs sh and git; skipped without them."""
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

DISCORD = Path(__file__).resolve().parent.parent / "tools" / "discord"
sys.path.insert(0, str(DISCORD))
import clinic_health as hl  # noqa: E402

SH = shutil.which("sh")
GIT = shutil.which("git")


def git(cwd, *a):
    env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")
    return subprocess.run([GIT, "-C", str(cwd), *a], check=True, stdout=subprocess.PIPE, env=env).stdout.decode().strip()


def test_record_running_commit(tmp_path):
    if not GIT:
        pytest.skip("no git")
    r = tmp_path / "r"
    r.mkdir()
    git(r, "init", "-q")
    (r / "a").write_text("1")
    git(r, "add", "a")
    git(r, "commit", "-qm", "one")
    conf = tmp_path / "conf"
    assert hl.record_running_commit(r, conf) == git(r, "rev-parse", "HEAD")
    assert (conf / "running_commit").read_text().strip() == git(r, "rev-parse", "HEAD")
    assert hl.record_running_commit(tmp_path / "nogit", conf) is None
    assert not (conf / "running_commit").exists()


@pytest.fixture
def world(tmp_path):
    if not (SH and GIT):
        pytest.skip("no sh/git")
    origin, repo = tmp_path / "origin.git", tmp_path / "repo"
    git(tmp_path, "init", "-q", "--bare", str(origin))
    git(tmp_path, "clone", "-q", str(origin), str(repo))
    d = repo / "tools" / "discord"
    d.mkdir(parents=True)
    (d / "keepalive.sh").write_text('echo started >> "$MARK/restarts"\n')
    (repo / "v").write_text("old")
    git(repo, "add", "-A")
    git(repo, "commit", "-qm", "old")
    git(repo, "push", "-q", "origin", "HEAD")
    old = git(repo, "rev-parse", "HEAD")
    other = tmp_path / "other"
    git(tmp_path, "clone", "-q", str(origin), str(other))
    (other / "v").write_text("new")
    git(other, "commit", "-qam", "new")
    git(other, "push", "-q", "origin", "HEAD")
    venv, conf, mark = tmp_path / "venv", tmp_path / "conf", tmp_path / "mark"
    (venv / "bin").mkdir(parents=True)
    conf.mkdir()
    mark.mkdir()
    py = venv / "bin" / "python"
    py.write_bytes(('#!/bin/sh\ncase "$*" in\n *"import pytest"*) exit 0;;\n *"-m pytest"*) exit "${FAKE_TESTS:-0}";;\n'
                  ' *clinic_health.py*) echo "$*" >> "$MARK/alerts"; exit 0;;\nesac\nexit 0\n').encode())
    py.chmod(0o755)
    env = dict(os.environ, REPO=repo.as_posix(), VENV=venv.as_posix(), NETDOCTOR_DIR=conf.as_posix(),
               MARK=mark.as_posix(), READY_WAIT="1")

    def deploy(*args, **extra):
        return subprocess.run([SH, (DISCORD / "deploy.sh").as_posix(), *args], env=dict(env, **extra),
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return repo, conf, mark, old, deploy


def test_dry_run_changes_nothing(world):
    repo, conf, mark, old, deploy = world
    (conf / "running_commit").write_text(old + "\n")
    r = deploy("--dry-run")
    assert git(repo, "rev-parse", "HEAD") == old and (repo / "v").read_text() == "old"
    assert not (mark / "restarts").exists() and not (mark / "alerts").exists()
    assert b"DRY RUN" in r.stdout


def test_tests_fail_rolls_back_to_running_commit_without_restart(world):
    repo, conf, mark, old, deploy = world
    # a previous run pulled already: HEAD is new, but the running bot started from `old`
    git(repo, "pull", "-q", "--ff-only")
    assert git(repo, "rev-parse", "HEAD") != old
    (conf / "running_commit").write_text(old + "\n")
    r = deploy(FAKE_TESTS="1")
    assert r.returncode == 1, r.stdout
    assert git(repo, "rev-parse", "HEAD") == old and (repo / "v").read_text() == "old", r.stdout
    assert not (mark / "restarts").exists()
    assert len((mark / "alerts").read_text().strip().splitlines()) == 1
    assert b"keeps" in r.stdout


def test_tests_fail_without_running_file_uses_head_before_pull(world):
    repo, conf, mark, old, deploy = world
    r = deploy(FAKE_TESTS="1")
    assert r.returncode == 1 and git(repo, "rev-parse", "HEAD") == old and not (mark / "restarts").exists()
