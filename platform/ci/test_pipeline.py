#!/usr/bin/env python3
"""Regression checks for release gates and complete legacy-demo coverage."""

import copy
import importlib.util
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

    release = publish["jobs"]["release"]
    condition = release["if"]
    assert "github.event.workflow_run.conclusion == 'success'" in condition, "release permits failed tests"
    assert "github.event.workflow_run.event == 'push'" in condition, "release permits untrusted event"
    assert "github.event.workflow_run.head_repository.full_name == github.repository" in condition, "release permits fork"
    checkout = next(step for step in release["steps"] if step.get("uses", "").startswith("actions/checkout@"))
    assert checkout["with"]["ref"] == "${{ github.event.workflow_run.head_sha }}", "release checks out wrong commit"
    download = next(step for step in release["steps"] if step.get("uses", "").startswith("actions/download-artifact@"))
    assert download["with"]["run-id"] == "${{ github.event.workflow_run.id }}", "release downloads wrong workflow run"
    assert download["with"]["pattern"] == "package-*", "release includes test data"


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
