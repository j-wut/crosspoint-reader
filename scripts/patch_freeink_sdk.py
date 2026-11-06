"""
PlatformIO pre-build script: apply scripts/freeink_sdk_patches/*.patch to the
freeink-sdk submodule working tree, in lexical order. Applied patches are
skipped; a patch that no longer applies fails the build. Before bumping the
submodule, revert with `git -C freeink-sdk checkout -- .` and refresh the patches.
"""

Import("env")  # noqa: F821 (SCons-injected global)
import os
import subprocess
import sys


PROJECT_DIR = env["PROJECT_DIR"]  # noqa: F821
PATCH_DIR = os.path.join(PROJECT_DIR, "scripts", "freeink_sdk_patches")
SDK_DIR = os.path.join(PROJECT_DIR, "freeink-sdk")


def _git_apply_succeeds(patch_path, *, reverse):
    cmd = ["git", "apply", "--check"]
    if reverse:
        cmd.append("--reverse")
    cmd.append(patch_path)
    return subprocess.run(cmd, cwd=SDK_DIR, capture_output=True, text=True).returncode == 0


def _apply_one(patch_path):
    name = os.path.basename(patch_path)
    if _git_apply_succeeds(patch_path, reverse=True):
        return
    if not _git_apply_succeeds(patch_path, reverse=False):
        result = subprocess.run(["git", "apply", "--check", patch_path], cwd=SDK_DIR, capture_output=True, text=True)
        sys.stderr.write("ERROR: FreeInk SDK patch %s does not apply cleanly (SDK bumped? refresh the patch):\n%s%s\n"
                         % (name, result.stdout, result.stderr))
        raise SystemExit(1)
    subprocess.run(["git", "apply", patch_path], cwd=SDK_DIR, check=True)
    print("Applied FreeInk SDK patch: %s" % name)


def patch_sdk():
    if not os.path.isdir(os.path.join(SDK_DIR, "libs")):
        sys.stderr.write("ERROR: freeink-sdk submodule is missing; run git submodule update --init\n")
        raise SystemExit(1)
    patches = sorted(os.path.join(PATCH_DIR, n) for n in os.listdir(PATCH_DIR) if n.endswith(".patch"))
    if not patches:
        sys.stderr.write("ERROR: no .patch files in %s\n" % PATCH_DIR)
        raise SystemExit(1)
    for patch in patches:
        _apply_one(patch)


patch_sdk()
