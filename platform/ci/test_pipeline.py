#!/usr/bin/env python3
"""Regression checks for release gates and complete legacy-demo coverage."""

import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import yaml

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
TARGETS = {
    "windows-x86_64", "windows-i686", "windows-xp-i686-nosse2", "windows-arm64",
    "debian12-amd64", "debian12-i386", "debian12-i386-nosse2", "debian12-armhf",
    "debian12-arm64", "linux-x86_64-appimage", "macos-universal", "freebsd-amd64",
    "netbsd-i386-nosse2",
}


def load_runner():
    spec = importlib.util.spec_from_file_location("ci_validate", ROOT / "platform/ci/validate.py")
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    return runner


def check_workflows(build, publish):
    assert set(build["on"]) == {"pull_request", "release", "workflow_dispatch"}, "unexpected build trigger"
    assert build["on"]["release"] == {"types": ["published"]}, "release trigger must be publication only"
    assert set(publish["on"]) == {"workflow_call"}, "publication must be called by the gated build"
    assert build["permissions"] == {"contents": "read"}, "build has unnecessary token permissions"
    assert publish["permissions"] == {"contents": "read"}, "publication has workflow-wide write permission"
    jobs = build["jobs"]

    def ancestors(name, visiting=()):
        assert name not in visiting, "cyclic job dependency"
        needs = jobs[name].get("needs", [])
        if isinstance(needs, str):
            needs = [needs]
        result = set(needs)
        for dependency in needs:
            assert dependency in jobs, f"unknown dependency: {dependency}"
            result.update(ancestors(dependency, visiting + (name,)))
        return result

    artifacts = set()
    for name, job in jobs.items():
        platform = str(job.get("runs-on", "")) + str(job.get("container", ""))
        assert "windows" not in platform.lower(), f"Windows CI image in job {name}"
        uploads = [step.get("with", {}).get("name", "") for step in job.get("steps", [])
                   if step.get("uses", "").startswith("actions/upload-artifact@")]
        packages = [item for item in uploads if item.startswith("package-")]
        if not packages:
            continue
        assert {"validate-demos", "tests"} <= ancestors(name), f"ungated package job: {name}"
        assert not job.get("continue-on-error"), f"allowed package failure: {name}"
        assert "always(" not in str(job.get("if", "")), f"package job bypasses failure: {name}"
        matrix = job.get("strategy", {}).get("matrix", {})
        targets = matrix.get("target", []) or [item["target"] for item in matrix.get("include", [])]
        for package in packages:
            if "${{ matrix.target }}" in package:
                artifacts.update(targets)
            else:
                artifacts.add(package.removeprefix("package-"))
    assert artifacts == TARGETS, f"wrong package set: {artifacts ^ TARGETS}"
    assert "validate-demos" in ancestors("tests"), "integration tests precede demo validation"
    expected = set(load_runner().SUITES) - {"demos", "unit"}
    assert set(jobs["tests"]["strategy"]["matrix"]["suite"]) == expected, "missing integration test shard"
    for name in ("validate-demos", "tests"):
        assert not jobs[name].get("continue-on-error"), f"allowed test failure: {name}"
        for step in jobs[name]["steps"]:
            assert not step.get("continue-on-error"), f"allowed test step failure: {name}"
    commands = "\n".join(step.get("run", "") for step in jobs["validate-demos"]["steps"])
    assert "--suite demos" in commands and "--suite unit" in commands, "missing initial validation gates"
    assert commands.index("--suite demos") < commands.index("--suite unit"), "demo gate must run first"

    caller = jobs["publish-release"]
    required_condition = "github.event_name == 'release' && github.event.action == 'published'"
    assert caller["if"] == required_condition, "publication permits non-release builds"
    assert caller["uses"] == "./.github/workflows/publish.yml", "wrong publication workflow"
    assert caller["needs"] == "packages-complete", "publication bypasses the complete-package gate"
    assert {"windows", "linux", "macos", "freebsd", "netbsd"} <= ancestors("packages-complete"), "incomplete package gate"
    assert not jobs["packages-complete"].get("continue-on-error"), "complete-package gate permits failure"
    assert not jobs["packages-complete"].get("if"), "complete-package gate bypasses package failures"
    gate_commands = "\n".join(step.get("run", "") for step in jobs["packages-complete"]["steps"])
    assert "package.py verify-release --directory dist" in gate_commands, "complete-package verification is missing"
    for step in jobs["packages-complete"]["steps"]:
        assert not step.get("continue-on-error"), "complete-package verification permits failure"
    for name, job in jobs.items():
        if name != "publish-release":
            assert job.get("permissions", {"contents": "read"}) == {"contents": "read"}, "non-release job has extra permissions"
    release_permissions = {"actions": "read", "contents": "write"}
    assert caller["permissions"] == release_permissions, "wrong publication token permissions"
    assert set(publish["jobs"]) == {"release"}, "unexpected publication jobs"
    release = publish["jobs"]["release"]
    assert release["permissions"] == release_permissions, "wrong reusable publication token permissions"
    assert release["if"] == required_condition, "reusable publication permits non-release builds"
    assert not release.get("continue-on-error"), "publication permits failure"
    steps = release["steps"]
    for step in steps:
        assert not step.get("continue-on-error"), "publication step permits failure"
        assert not step.get("if"), "publication step can bypass verification"
    checkout = next(step for step in steps if step.get("uses", "").startswith("actions/checkout@"))
    assert checkout["with"]["ref"] == "${{ github.sha }}", "release checks out wrong commit"
    download = next(step for step in steps if step.get("uses", "").startswith("actions/download-artifact@"))
    assert download["with"]["run-id"] == "${{ github.run_id }}", "release downloads wrong workflow run"
    assert download["with"]["pattern"] == "package-*", "release includes test data"
    verification = next(step for step in steps if "package.py verify-release" in step.get("run", ""))
    tag_check = next(step for step in steps if "release_revision=" in step.get("run", ""))
    assert tag_check["env"]["SOURCE_REVISION"] == "${{ github.sha }}", "release verifies wrong commit"
    assert tag_check["env"]["RELEASE_ID"] == "${{ github.event.release.id }}", "release verifies wrong release"
    assert tag_check["env"]["RELEASE_TAG"] == "${{ github.event.release.tag_name }}", "release verifies wrong tag"
    upload = next(step for step in steps if "gh release upload" in step.get("run", ""))
    assert upload["env"]["RELEASE_TAG"] == "${{ github.event.release.tag_name }}", "release uploads to wrong tag"
    assert steps.index(verification) < steps.index(tag_check) < steps.index(upload), "publication bypasses verification"
    commands = "\n".join(line for step in steps for line in step.get("run", "").splitlines()
                         if not line.lstrip().startswith("#"))
    assert "gh release create" not in commands, "publication creates a release"
    assert "--clobber" not in commands, "publication overwrites existing assets"


class PipelineTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = yaml.safe_load((ROOT / ".github/workflows/build.yml").read_text())
        cls.publish = yaml.safe_load((ROOT / ".github/workflows/publish.yml").read_text())

    def test_release_contract(self):
        check_workflows(self.build, self.publish)

    def test_missing_gate_is_rejected(self):
        broken = copy.deepcopy(self.build)
        broken["jobs"]["windows"].pop("needs")
        with self.assertRaises(AssertionError):
            check_workflows(broken, self.publish)

    def test_missing_target_is_rejected(self):
        broken = copy.deepcopy(self.build)
        broken["jobs"]["windows"]["strategy"]["matrix"]["target"].pop()
        with self.assertRaises(AssertionError):
            check_workflows(broken, self.publish)

    def test_missing_runtime_suite_is_rejected(self):
        broken = copy.deepcopy(self.build)
        broken["jobs"]["tests"]["strategy"]["matrix"]["suite"].pop()
        with self.assertRaises(AssertionError):
            check_workflows(broken, self.publish)

    def test_windows_runner_is_rejected(self):
        broken = copy.deepcopy(self.build)
        broken["jobs"]["windows"]["runs-on"] = "windows-latest"
        with self.assertRaises(AssertionError):
            check_workflows(broken, self.publish)

    def test_unpinned_release_run_is_rejected(self):
        broken = copy.deepcopy(self.publish)
        download = next(step for step in broken["jobs"]["release"]["steps"]
                        if step.get("uses", "").startswith("actions/download-artifact@"))
        download["with"]["run-id"] = "latest"
        with self.assertRaises(AssertionError):
            check_workflows(self.build, broken)

    def test_automatic_push_and_merge_queue_triggers_are_rejected(self):
        for trigger in ("push", "merge_group"):
            with self.subTest(trigger=trigger):
                broken = copy.deepcopy(self.build)
                broken["on"][trigger] = {}
                with self.assertRaises(AssertionError):
                    check_workflows(broken, self.publish)

    def test_other_release_activity_is_rejected(self):
        broken = copy.deepcopy(self.build)
        broken["on"]["release"]["types"].append("created")
        with self.assertRaises(AssertionError):
            check_workflows(broken, self.publish)

    def test_ungated_or_non_release_publication_is_rejected(self):
        mutations = (("if", "always()"), ("needs", "tests"))
        for key, value in mutations:
            with self.subTest(key=key):
                broken = copy.deepcopy(self.build)
                broken["jobs"]["publish-release"][key] = value
                with self.assertRaises(AssertionError):
                    check_workflows(broken, self.publish)
        broken = copy.deepcopy(self.publish)
        broken["jobs"]["release"]["if"] = "always()"
        with self.assertRaises(AssertionError):
            check_workflows(self.build, broken)

    def test_independent_publication_trigger_is_rejected(self):
        broken = copy.deepcopy(self.publish)
        broken["on"]["workflow_run"] = {"workflows": ["Build"], "types": ["completed"]}
        with self.assertRaises(AssertionError):
            check_workflows(self.build, broken)

    def test_unpinned_release_commit_is_rejected(self):
        broken = copy.deepcopy(self.publish)
        checkout = next(step for step in broken["jobs"]["release"]["steps"]
                        if step.get("uses", "").startswith("actions/checkout@"))
        checkout["with"]["ref"] = "master"
        with self.assertRaises(AssertionError):
            check_workflows(self.build, broken)

    def test_release_creation_or_overwrite_is_rejected(self):
        for command in ('gh release create "$RELEASE_TAG"', 'gh release upload --clobber "$RELEASE_TAG"'):
            with self.subTest(command=command):
                broken = copy.deepcopy(self.publish)
                broken["jobs"]["release"]["steps"][-1]["run"] += "\n" + command + "\n"
                with self.assertRaises(AssertionError):
                    check_workflows(self.build, broken)

    def test_non_release_write_permission_is_rejected(self):
        broken = copy.deepcopy(self.build)
        broken["jobs"]["tests"]["permissions"] = {"contents": "write"}
        with self.assertRaises(AssertionError):
            check_workflows(broken, self.publish)

    def test_published_release_identity_and_tag_gate(self):
        # Run the actual shell gate offline, replacing only GitHub/Git access.
        # No network calls, repository refs, releases, or uploaded assets change.
        step = next(step for step in self.publish["jobs"]["release"]["steps"]
                    if "release_revision=" in step.get("run", ""))
        release = {"id": 42, "tag_name": "v1.0", "draft": False, "published_at": "2026-10-14T00:00:00Z"}
        cases = (
            ("published", {}, "a" * 40, False, True),
            ("moved-tag", {}, "b" * 40, False, False),
            ("draft", {"draft": True}, "a" * 40, False, False),
            ("unpublished", {"published_at": None}, "a" * 40, False, False),
            ("renamed-tag", {"tag_name": "v2.0"}, "a" * 40, False, False),
            ("replaced-release", {"id": 43}, "a" * 40, False, False),
            ("deleted-release", {}, "a" * 40, True, False),
        )
        with tempfile.TemporaryDirectory(prefix="netblood-release-gate-") as directory:
            directory = Path(directory)
            stub = """#!/usr/bin/env python3
import os
from pathlib import Path
import sys
if Path(sys.argv[0]).name == "gh":
    assert sys.argv[1:] == ["api", "repos/test/NetBlood/releases/42"]
    if os.environ["MISSING_RELEASE"] == "1":
        raise SystemExit(1)
    print(os.environ["RELEASE_JSON"])
else:
    command = sys.argv[1:]
    assert command in (["check-ref-format", "refs/tags/v1.0"],
                       ["fetch", "--force", "origin", "refs/tags/v1.0:refs/tags/v1.0"],
                       ["rev-parse", "refs/tags/v1.0^{commit}"])
    if command[0] == "rev-parse":
        print(os.environ["TAG_REVISION"])
"""
            for name in ("gh", "git"):
                executable = directory / name
                executable.write_text(stub)
                executable.chmod(0o755)
            for name, changes, revision, missing, expected in cases:
                with self.subTest(case=name):
                    env = dict(os.environ, PATH=str(directory) + os.pathsep + os.environ["PATH"],
                               RUNNER_TEMP=str(directory), GITHUB_REPOSITORY="test/NetBlood",
                               RELEASE_ID="42", RELEASE_TAG="v1.0", SOURCE_REVISION="a" * 40,
                               RELEASE_JSON=json.dumps(dict(release, **changes)), TAG_REVISION=revision,
                               MISSING_RELEASE="1" if missing else "0")
                    result = subprocess.run(["bash", "-c", step["run"]], env=env, text=True,
                                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10)
                    self.assertEqual(result.returncode == 0, expected, result.stdout)

    def test_strict_demo_gate(self):
        # This fake executable only supplies process output. No Blood assets or
        # gameplay results are fabricated for the actual 144-demo CI run.
        with tempfile.TemporaryDirectory(prefix="netblood-ci-gate-") as directory:
            directory = Path(directory)
            assets = directory / "asset-placeholders"
            assets.mkdir()
            required = ["BLOOD.INI", "BLOOD.RFF", "GUI.RFF", "SOUNDS.RFF", "SURFACE.DAT", "VOXEL.DAT"]
            required += [f"TILES{i:03}.ART" for i in range(18)]
            for name in required:
                (assets / name).write_bytes(b"gate-parser-test; not game data")
            cases = (
                ("complete", list(range(144)), 0, 0),
                ("excluded", list(range(143)), 1, 1),
                ("duplicate", [0] * 144, 0, 1),
                ("wrong-id", list(range(1, 145)), 0, 1),
            )
            for name, identifiers, skipped, expected in cases:
                with self.subTest(case=name):
                    binary = directory / name
                    log = "\n".join(f"Demo Synced: /validatedemos/TEST{i:03}.DEM" for i in identifiers)
                    log += f"\n1.21 Validation Successful! ({len(identifiers)} passed, {skipped} skipped)\n"
                    binary.write_text(f"#!{sys.executable}\nprint({log!r})\n")
                    binary.chmod(0o755)
                    result = subprocess.run(
                        [sys.executable, str(ROOT / "tests/demo-validation/run.py"), "--require-all",
                         "--binary", str(binary), "--data-dir", str(assets), "--output-dir", str(directory / "evidence")],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=30,
                    )
                    self.assertEqual(result.returncode, expected, result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
