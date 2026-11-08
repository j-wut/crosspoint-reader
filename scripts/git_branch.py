"""
PlatformIO pre-build script: set CROSSPOINT_VERSION only on the source files
that use it.

Development environments get the git branch and short SHA, giving a version
string like:  1.1.0-dev-feat-kosync-xpath-05c6cf8
Release environments set CROSSPOINT_VERSION in the ini. That define is moved
out of the global flags here, so a new release candidate hash does not change
the compile command of every file and the build cache stays valid.
"""

import configparser
import os
import re
import subprocess
import sys


def warn(msg):
    print(f'WARNING [git_branch.py]: {msg}', file=sys.stderr)


def run_git_value(project_dir, args, label):
    try:
        value = subprocess.check_output(
            ['git', *args],
            text=True, stderr=subprocess.PIPE, cwd=project_dir
        ).strip()
        # Strip characters that would break a C string literal
        return ''.join(c for c in value if c not in '"\\')
    except FileNotFoundError:
        warn(f'git not found on PATH; {label} suffix will be "unknown"')
        return 'unknown'
    except subprocess.CalledProcessError as e:
        warn(
            f'git command failed (exit {e.returncode}): '
            f'{e.stderr.strip()}; {label} suffix will be "unknown"'
        )
        return 'unknown'
    except OSError as e:
        warn(
            f'OS error reading git {label}: {e}; '
            f'{label} suffix will be "unknown"'
        )
        return 'unknown'
    except Exception as e:  # pylint: disable=broad-exception-caught
        warn(
            f'Unexpected error reading git {label}: {e}; '
            f'{label} suffix will be "unknown"'
        )
        return 'unknown'


def get_git_branch(project_dir):
    branch = run_git_value(
        project_dir, ['rev-parse', '--abbrev-ref', 'HEAD'], 'branch'
    )
    # Detached HEAD has no branch name.
    if branch == 'HEAD':
        return 'detached'
    return branch


def get_git_short_sha(project_dir):
    return run_git_value(
        project_dir, ['rev-parse', '--short', 'HEAD'], 'short SHA'
    )


def get_base_version(project_dir):
    ini_path = os.path.join(project_dir, 'platformio.ini')
    if not os.path.isfile(ini_path):
        warn(f'platformio.ini not found at {ini_path}; base version will be "0.0.0"')
        return '0.0.0'
    config = configparser.ConfigParser()
    config.read(ini_path, encoding='utf-8')
    if not config.has_option('crosspoint', 'version'):
        warn('No [crosspoint] version in platformio.ini; base version will be "0.0.0"')
        return '0.0.0'
    return config.get('crosspoint', 'version')


def take_version_flag(env):
    """Remove -DCROSSPOINT_VERSION from BUILD_FLAGS and return its value."""
    kept = []
    value = None
    for flag in env.get('BUILD_FLAGS', []):
        if re.match(r'\s*-DCROSSPOINT_VERSION=', str(flag)):
            # Parse it the same way PlatformIO parses build_flags.
            value = env.ParseFlagsExtended([flag])['CPPDEFINES'][0][1]
        else:
            kept.append(flag)
    if value is not None:
        env.Replace(BUILD_FLAGS=kept)
    return value


def inject_version(env):
    version_define = take_version_flag(env)
    if version_define is None:
        # Release envs set the version in platformio.ini. Development envs
        # build it from Git metadata.
        if env['PIOENV'] not in ('default', 'sticky'):
            return
        project_dir = env['PROJECT_DIR']
        base_version = get_base_version(project_dir)
        branch = get_git_branch(project_dir)
        short_sha = get_git_short_sha(project_dir)
        version_string = f'{base_version}-dev-{branch}-{short_sha}'
        version_define = f'\\"{version_string}\\"'
        print(f'CrossPoint build version: {version_string}')

    def add_version(build_env, node):
        # Only these translation units depend on the version. Keep library and
        # reader objects reusable when the branch or commit changes.
        if b'CROSSPOINT_VERSION' not in node.srcnode().get_contents():
            return node
        version_env = build_env.Clone()
        version_env.Append(CPPDEFINES=[('CROSSPOINT_VERSION', version_define)])
        return version_env.Object(node)

    env.AddBuildMiddleware(add_version)


# PlatformIO/SCons entry point — Import and env are SCons builtins injected at runtime.
# When run directly with Python (e.g. for validation), a lightweight fake env is used
# so the git/version logic can be exercised without a full build.
try:
    Import('env')           # noqa: F821  # type: ignore[name-defined]
    inject_version(env)     # noqa: F821  # type: ignore[name-defined]
except NameError:
    class _Env(dict):
        def AddBuildMiddleware(self, _):
            # Validation mode only reports the computed version.
            return None

    _project_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    inject_version(_Env({'PIOENV': 'default', 'PROJECT_DIR': _project_dir}))
