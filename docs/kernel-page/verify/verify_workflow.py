#!/usr/bin/env python3
"""Exercise the committed workflow's failure paths hermetically.

Extract a commit with git archive, copy the pinned firmware and kernel source,
then drive the workflow's own hash gate and verifier block through five
mutations. Every case must fail at the expected point; a fresh extraction must
pass after each one. Nothing is written outside a temporary directory.
"""
import argparse
import io
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

WORKFLOW = pathlib.Path(".github/workflows/kernel-page-verification.yml")
CAPTURES = pathlib.Path("docs/kernel-page/logs/20261104_enumeration/bodies")
MISSING_CAPTURE = CAPTURES / "347_20261101_143931_KP.txt"
CORRUPT_CAPTURE = CAPTURES / "387_20261102_134935_KP.txt"
VERIFIERS = ("verify_symbol", "verify_boundary", "verify_timeline", "verify_lifecycle")


class HarnessError(RuntimeError):
    pass


def extract(commit, source, dest, env):
    run = subprocess.run(
        ["git", "-C", str(source), "archive", "--format=tar", commit],
        env=env, capture_output=True,
    )
    if run.returncode:
        raise HarnessError(run.stderr.decode("utf-8", "replace").strip())
    with tarfile.open(fileobj=io.BytesIO(run.stdout)) as archive:
        archive.extractall(dest, filter="data")


def run_block(workflow, name):
    """Return the shell text of one named step's run: entry."""
    lines = workflow.splitlines()
    start = next(
        (i for i, line in enumerate(lines) if line.strip() == "- name: " + name),
        None,
    )
    if start is None:
        raise HarnessError("workflow step not found: " + name)
    end = next(
        (i for i in range(start + 1, len(lines))
         if lines[i].lstrip().startswith("- name:")),
        len(lines),
    )
    for i in range(start, end):
        match = re.match(r"^(\s*)run:\s*(.*)$", lines[i])
        if not match:
            continue
        indent, value = len(match.group(1)), match.group(2).strip()
        if value and value != "|":
            return value
        body = []
        for line in lines[i + 1:end]:
            if line.strip() and len(line) - len(line.lstrip()) <= indent:
                break
            body.append(line[indent + 2:] if len(line) >= indent + 2 else "")
        return "\n".join(body).rstrip() + "\n"
    raise HarnessError("workflow step has no run: block: " + name)


def copy_inputs(fw, ksrc, dest):
    shutil.copytree(fw, dest / "fw")
    shutil.copytree(ksrc, dest / "ksrc")


def shell(script, cwd, env):
    return subprocess.run(
        ["bash", "--noprofile", "--norc", "-c", script],
        cwd=cwd, env=env, capture_output=True, text=True,
    )


def one_line(proc):
    text = (proc.stdout + proc.stderr).strip()
    for line in text.splitlines():
        if any(key in line for key in
               ("FAILED", "No such file", "did NOT match", "::error", "problems:")):
            return line.strip()
    return text.splitlines()[-1].strip() if text else ""


def assert_case(name, gate, verifiers, point, needles, run_root):
    output = gate.stdout + gate.stderr
    if point == "gate":
        if gate.returncode == 0:
            raise HarnessError(name + ": hash gate accepted the mutation")
        if any((run_root / (v + ".log")).exists() for v in VERIFIERS):
            raise HarnessError(name + ": verifier ran after a failed hash gate")
        missing = [needle for needle in needles if needle not in output]
        if missing:
            raise HarnessError(name + ": gate output lacks " + repr(missing))
        return "gate rc=%d; %s" % (gate.returncode, one_line(gate))

    if gate.returncode != 0:
        raise HarnessError(name + ": gate should pass, got rc=%d" % gate.returncode)
    if verifiers.returncode == 0:
        raise HarnessError(name + ": verifier block accepted the mutation")
    logs = sorted((run_root / (v + ".log")).name for v in VERIFIERS)
    if logs != sorted(v + ".log" for v in VERIFIERS):
        raise HarnessError(name + ": missing verifier logs: " + repr(logs))
    symbol = (run_root / "verify_symbol.log").read_text()
    step_output = verifiers.stdout + verifiers.stderr
    if "problems: 0" in symbol:
        raise HarnessError(name + ": verify_symbol log did not record failures")
    if "::error title=verify_symbol" not in step_output:
        raise HarnessError(name + ": verify_symbol failure was not attributed")
    other_errors = [line for line in step_output.splitlines()
                    if line.startswith("::error") and "verify_symbol" not in line]
    if other_errors:
        raise HarnessError(name + ": unexpected verifier failure: " + other_errors[0])
    missing = [needle for needle in needles if needle not in symbol]
    if missing:
        raise HarnessError(name + ": verify_symbol log lacks " + repr(missing))
    failure = next((line for line in step_output.splitlines()
                    if line.startswith("::error")), one_line(verifiers))
    return "verify_symbol rc=%d; %s" % (verifiers.returncode, failure)


def append_byte(path):
    with path.open("ab") as handle:
        handle.write(b"x")


def cases():
    return (
        ("missing firmware input", "gate",
         lambda work, inputs: (inputs / "fw/kernel.bin").unlink(),
         ("No such file", "fw/kernel.bin")),
        ("wrong firmware hash", "gate",
         lambda work, inputs: append_byte(inputs / "fw/kernel.bin"),
         ("FAILED", "fw/kernel.bin")),
        ("wrong kernel source revision", "gate",
         lambda work, inputs: append_byte(inputs / "ksrc/kernel/locking/rtmutex.c"),
         ("FAILED", "rtmutex.c")),
        ("missing capture artifact", "verify_symbol",
         lambda work, inputs: (work / MISSING_CAPTURE).unlink(),
         ("FAIL", "hash target missing", "347")),
        ("corrupted evidence file", "verify_symbol",
         lambda work, inputs: append_byte(work / CORRUPT_CAPTURE),
         ("FAIL", "hash matches", "387")),
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fw", required=True, type=pathlib.Path)
    parser.add_argument("--ksrc", required=True, type=pathlib.Path)
    parser.add_argument("--source")
    parser.add_argument("--commit", default="HEAD")
    args = parser.parse_args()
    for path, label in ((args.fw, "--fw"), (args.ksrc, "--ksrc")):
        if not path.is_dir():
            parser.error(label + " must be a directory: " + str(path))
    if args.source:
        source = pathlib.Path(args.source)
    else:
        try:
            source = pathlib.Path(__file__).resolve().parents[3]
        except IndexError:
            parser.error("--source is required when the script is outside the repository")

    with tempfile.TemporaryDirectory(prefix="kernel-page-mutations-") as tmp:
        root = pathlib.Path(tmp)
        home = root / "home"
        home.mkdir()
        print("commit:", args.commit, "source:", source)

        def fresh(label):
            run_root = pathlib.Path(tempfile.mkdtemp(prefix=label + "-", dir=root))
            work = run_root / "work"
            work.mkdir()
            env = dict(os.environ)
            env.update({
                "HOME": str(home),
                "RUNNER_TEMP": str(run_root),
                "GITHUB_WORKSPACE": str(work),
                "PYTHONDONTWRITEBYTECODE": "1",
            })
            for name in ("BASH_ENV", "ENV", "PYTHONPATH", "PYTHONHOME",
                         "GIT_DIR", "GIT_WORK_TREE"):
                env.pop(name, None)
            extract(args.commit, source, work, env)
            inputs = run_root / "ks-inputs"
            copy_inputs(args.fw, args.ksrc, inputs)
            return run_root, work, inputs, env

        def run_paths(work, inputs, env):
            workflow = (work / WORKFLOW).read_text()
            gate = shell(run_block(workflow, "Verify pinned input hashes"), inputs, env)
            verifiers = shell(run_block(workflow, "Run all four verifiers"), work, env)
            return gate, verifiers

        def baseline(label):
            run_root, work, inputs, env = fresh(label)
            gate, verifiers = run_paths(work, inputs, env)
            if gate.returncode != 0:
                raise HarnessError(label + ": baseline hash gate failed: " + one_line(gate))
            if verifiers.returncode != 0:
                raise HarnessError(label + ": baseline verifier failed: " + one_line(verifiers))
            logs = sorted((run_root / (v + ".log")).name for v in VERIFIERS)
            if logs != sorted(v + ".log" for v in VERIFIERS):
                raise HarnessError(label + ": baseline logs incomplete: " + repr(logs))
            print("baseline: rc=0; four verifiers green")

        baseline("baseline")
        for name, point, mutate, needles in cases():
            run_root, work, inputs, env = fresh("mutated")
            mutate(work, inputs)
            workflow = (work / WORKFLOW).read_text()
            gate = shell(run_block(workflow, "Verify pinned input hashes"), inputs, env)
            verifiers = None
            if gate.returncode == 0:
                verifiers = shell(run_block(workflow, "Run all four verifiers"), work, env)
            detail = assert_case(name, gate, verifiers, point, needles, run_root)
            print("%s: %s" % (name, detail))
            baseline("restored")

        print("all five mutations failed closed; all six baselines green")
        return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except HarnessError as error:
        print("mutation harness failed:", error, file=sys.stderr)
        sys.exit(1)
