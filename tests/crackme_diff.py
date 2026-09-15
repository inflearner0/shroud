#!/usr/bin/env python3
import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path


def exe_suffix():
    return ".exe" if platform.system() == "Windows" else ""


def build(clang, src, out, plugin=None, seed=None):
    cmd = [clang, "-O2", "-Wall", "-Wextra", "-fno-vectorize", "-fno-slp-vectorize"]
    if plugin:
        cmd.append("-fpass-plugin=" + str(plugin))
    cmd += ["-o", str(out), str(src), "-lm"]
    env = dict(os.environ)
    env["SHROUD_VERBOSE"] = "1"
    if seed is not None:
        env["SHROUD_SEED"] = seed
    proc = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        sys.exit("build failed:\n" + proc.stderr.decode(errors="replace"))
    return proc.stderr.decode(errors="replace")


def run_exe(path, args):
    proc = subprocess.run([str(path)] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    normalized = proc.stdout.replace(str(path).encode(), b"<prog>")
    return proc.returncode, normalized


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--clang", required=True)
    parser.add_argument("--plugin", required=True)
    parser.add_argument("--program", required=True)
    parser.add_argument("--workdir", required=True)
    parser.add_argument("--seed", default="0x1337")
    parser.add_argument("--key", default="Shr0udVM-Cr4ckm3")
    args = parser.parse_args()

    workdir = Path(args.workdir)
    workdir.mkdir(parents=True, exist_ok=True)
    baseline = workdir / ("baseline" + exe_suffix())
    obfuscated = workdir / ("obfuscated" + exe_suffix())

    build(args.clang, args.program, baseline)
    log = build(args.clang, args.program, obfuscated, plugin=args.plugin, seed=args.seed)

    vm_count = len(re.findall(r"vm\(cells=", log))
    if vm_count < 4:
        sys.exit("expected at least 4 VM functions, got {0}\n{1}".format(vm_count, log))

    cases = [
        [args.key],
        [args.key[:-1] + "4"],
        ["unlock-ALT-0001!"],
        ["AAAAAAAAAAAAAAAA"],
        ["short"],
        [""],
        [],
    ]

    for case in cases:
        base_code, base_out = run_exe(baseline, case)
        obf_code, obf_out = run_exe(obfuscated, case)
        if (base_code, base_out) != (obf_code, obf_out):
            sys.exit(
                "mismatch for args={0}\nbaseline:   {1} {2!r}\nobfuscated: {3} {4!r}".format(
                    case, base_code, base_out, obf_code, obf_out
                )
            )

    granted_code, granted_out = run_exe(obfuscated, [args.key])
    denied_code, denied_out = run_exe(obfuscated, [args.key + "x"])
    if granted_code != 0 or b"Access granted" not in granted_out:
        sys.exit("correct key did not grant access: {0!r}".format(granted_out))
    if denied_code == 0 or b"Access denied" not in denied_out:
        sys.exit("bad key was not denied: {0!r}".format(denied_out))

    if shutil.which("gdb"):
        try:
            proc = subprocess.run(
                ["gdb", "-batch", "-ex", "run", "-ex", "quit", "--args",
                 str(obfuscated), args.key],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=120,
            )
            if b"Access granted" in proc.stdout:
                sys.exit("anti-debug failure: correct key granted access under gdb")
            print("anti-debug ok: access denied while under gdb")
        except subprocess.TimeoutExpired:
            print("anti-debug ok: run under gdb did not finish")
        except Exception as exc:
            print("anti-debug check skipped: " + str(exc))

    print(
        "crackme ok: vm={0} | granted={1!r} | denied exit={2}".format(
            vm_count, granted_out.decode().splitlines()[0], denied_code
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
