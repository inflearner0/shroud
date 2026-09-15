#!/usr/bin/env python3
import argparse
import hashlib
import os
import platform
import re
import subprocess
import sys
from pathlib import Path


def exe_suffix():
    return ".exe" if platform.system() == "Windows" else ""


def build(clang, src, out, plugin=None, seed=None, cflags=None, profile=None):
    cmd = [clang, "-O2", "-Wall", "-Wextra"]
    if cflags:
        cmd += list(cflags)
    if plugin:
        cmd.append("-fpass-plugin=" + str(plugin))
    cmd += ["-o", str(out), str(src)]
    if platform.system() != "Windows":
        cmd.append("-lm")
    env = dict(os.environ)
    env["SHROUD_VERBOSE"] = "1"
    if seed is not None:
        env["SHROUD_SEED"] = str(seed)
    if profile is not None:
        env["SHROUD_PROFILE"] = str(profile)
    proc = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        sys.exit(
            "build failed with code {0}:\n{1}\n{2}".format(
                proc.returncode,
                " ".join(cmd),
                proc.stderr.decode(errors="replace"),
            )
        )
    return proc.stderr.decode(errors="replace")


def run_exe(path):
    proc = subprocess.run([str(path)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return proc.returncode, proc.stdout


def file_hash(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--clang", required=True)
    parser.add_argument("--plugin", required=True)
    parser.add_argument("--program", required=True)
    parser.add_argument("--workdir", required=True)
    parser.add_argument("--seed", default="0xC0FFEE")
    parser.add_argument("--min-transformed", type=int, default=0)
    parser.add_argument("--min-vm", type=int, default=0)
    parser.add_argument("--cflag", action="append", default=[])
    parser.add_argument("--profile", default=None)
    args = parser.parse_args()

    workdir = Path(args.workdir)
    workdir.mkdir(parents=True, exist_ok=True)
    baseline = workdir / ("baseline" + exe_suffix())
    obfuscated = workdir / ("obfuscated" + exe_suffix())

    build(args.clang, args.program, baseline, cflags=args.cflag)
    log = build(
        args.clang,
        args.program,
        obfuscated,
        plugin=args.plugin,
        seed=args.seed,
        cflags=args.cflag,
        profile=args.profile,
    )

    base_code, base_out = run_exe(baseline)
    obf_code, obf_out = run_exe(obfuscated)

    if base_code != obf_code:
        sys.exit("exit code mismatch: baseline={0} obfuscated={1}".format(base_code, obf_code))

    if base_out != obf_out:
        base_lines = base_out.decode(errors="replace").splitlines()
        obf_lines = obf_out.decode(errors="replace").splitlines()
        for i, (a, b) in enumerate(zip(base_lines, obf_lines)):
            if a != b:
                sys.exit(
                    "output mismatch at line {0}:\n  baseline:   {1}\n  obfuscated: {2}".format(
                        i + 1, a, b
                    )
                )
        sys.exit(
            "output length mismatch: baseline={0} lines, obfuscated={1} lines".format(
                len(base_lines), len(obf_lines)
            )
        )

    match = re.search(r"transformed (\d+) function", log)
    transformed = int(match.group(1)) if match else 0
    if transformed < args.min_transformed:
        sys.exit(
            "expected at least {0} transformed function(s), got {1}\nplugin log:\n{2}".format(
                args.min_transformed, transformed, log
            )
        )

    vm_count = len(re.findall(r"vm\(cells=", log))
    if vm_count < args.min_vm:
        sys.exit(
            "expected at least {0} VM-compiled function(s), got {1}\nplugin log:\n{2}".format(
                args.min_vm, vm_count, log
            )
        )

    if "unknown:" in log:
        sys.exit("annotation parse produced unknown tokens:\n" + log)

    if transformed > 0:
        same_seed = workdir / ("obfuscated_same" + exe_suffix())
        other_seed = workdir / ("obfuscated_seed2" + exe_suffix())
        build(args.clang, args.program, same_seed, plugin=args.plugin, seed=args.seed,
              cflags=args.cflag, profile=args.profile)
        build(
            args.clang,
            args.program,
            other_seed,
            plugin=args.plugin,
            seed=str(int(args.seed, 0) + 1),
            cflags=args.cflag,
            profile=args.profile,
        )
        if file_hash(obfuscated) != file_hash(same_seed):
            sys.exit("output is not deterministic for a fixed seed")
        if file_hash(obfuscated) == file_hash(other_seed):
            sys.exit("output does not vary with SHROUD_SEED")

    digest = ""
    for line in reversed(base_out.decode(errors="replace").splitlines()):
        if line.startswith("DIGEST"):
            digest = line
            break

    print(
        "differential ok: {0} | transformed={1} | vm={2} | {3}".format(
            Path(args.program).name, transformed, vm_count, digest
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
