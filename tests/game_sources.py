"""Read game files for tests from the player's own Space Marine 2 install.

The game's .sso scripts are Saber's content and are not kept in this repo. Tests that need
them read the same pak the pack builders read (default_other.pak) and skip when the game
isn't installed. The install comes from SM2VR_GAME_ROOT, or Steam (app 2183900), as in
installer/app/sm2-game.ps1.
"""
import functools
import os
from pathlib import Path
import re
import unittest
import zipfile

APP_ID = "2183900"
PAKS = Path("client_pc/root/paks/client/default")


def _steam_libraries():
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as key:
            steam = Path(winreg.QueryValueEx(key, "SteamPath")[0])
    except (ImportError, OSError):
        return []
    vdf = steam / "steamapps/libraryfolders.vdf"
    try:
        text = vdf.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return [steam]
    libraries = [steam]
    # Each library block is "path" "<dir>" followed by an "apps" block listing app ids.
    for block in re.finditer(r'"path"\s+"([^"]+)"(.*?)(?="path"|\Z)', text, re.S):
        if re.search(r'"%s"' % APP_ID, block.group(2)):
            libraries.insert(0, Path(block.group(1).replace("\\\\", "\\")))
    return libraries


@functools.cache
def game_root():
    """The game's install folder, or None."""
    override = os.environ.get("SM2VR_GAME_ROOT")
    candidates = [Path(override)] if override else [lib / "steamapps/common/Space Marine 2" for lib in _steam_libraries()]
    for root in candidates:
        if (root / PAKS / "default_other.pak").is_file():
            return root
    return None


@functools.cache
def _read(pak, name):
    with zipfile.ZipFile(game_root() / PAKS / pak) as z:
        return z.read(name)


def require_game():
    """Skip the calling test module when the game isn't installed."""
    if game_root() is None:
        raise unittest.SkipTest("Space Marine 2 is not installed (set SM2VR_GAME_ROOT); game-file tests skipped")


def game_text(name, pak="default_other.pak"):
    """A pak entry as UTF-8 text with LF line ends, e.g. game_text('ssl/input/keymap.sso')."""
    return _read(pak, name).decode("utf-8").replace("\r\n", "\n")
