"""PowerShell 7 for tests that run the dev launch scripts.

The scripts under scripts/ target PowerShell 7 (pwsh). On a machine without it, the tests that run
them skip instead of failing; install PowerShell 7 to run them.
"""
import shutil
import unittest


def pwsh():
    """Path to pwsh, or skip the calling test."""
    path = shutil.which("pwsh")
    if not path:
        raise unittest.SkipTest("PowerShell 7 (pwsh) is not installed")
    return path
