#!/usr/bin/env python3
import os
import shutil
import subprocess
import sys


def main():
    plugin = os.environ.get("SHROUD_PLUGIN")
    clang = os.environ.get("SHROUD_CLANG", "clang")
    profile = os.environ.get("SHROUD_PROFILE")
    args = sys.argv[1:]

    if clang == "clang":
        for candidate in ("clang", "clang-18"):
            if shutil.which(candidate):
                clang = candidate
                break

    while args and args[0].startswith("--"):
        option = args.pop(0)
        if option.startswith("--plugin="):
            plugin = option.split("=", 1)[1]
        elif option.startswith("--clang="):
            clang = option.split("=", 1)[1]
        elif option.startswith("--profile="):
            profile = option.split("=", 1)[1]
        else:
            sys.exit("unknown option: " + option)

    if not plugin:
        sys.exit("set SHROUD_PLUGIN or pass --plugin=<path to shroud plugin>")
    if not args:
        sys.exit("usage: shroudc.py [--plugin=...] [--clang=...] [--profile=...] <clang args>")

    env = dict(os.environ)
    if profile:
        env["SHROUD_PROFILE"] = profile
    cmd = [clang, "-fpass-plugin=" + plugin] + args
    return subprocess.call(cmd, env=env)


if __name__ == "__main__":
    raise SystemExit(main())
