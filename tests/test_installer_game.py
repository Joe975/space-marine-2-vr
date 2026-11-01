"""Game discovery through Steam's library files, against fixture trees."""
from pathlib import Path
import struct
import tempfile
import unittest

from installer_support import FIXTURES, TEMP, run_ps, run_ps_json

EXE = 'Warhammer 40000 Space Marine 2 - Retail.exe'


def fake_exe(path, timestamp):
    """Smallest file Get-Sm2PeTimestamp accepts: MZ header, e_lfanew, PE signature, file header."""
    data = bytearray(0x100)
    data[0:2] = b'MZ'
    data[0x3C:0x40] = struct.pack('<I', 0x80)
    data[0x80:0x84] = b'PE\0\0'
    data[0x88:0x8C] = struct.pack('<I', timestamp)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(data))


def install_game(library, installdir='Space Marine 2', buildid='26074365', timestamp=0x6ad10fcb):
    apps = library / 'steamapps'
    apps.mkdir(parents=True, exist_ok=True)
    (apps / 'appmanifest_2183900.acf').write_text(
        '"AppState"\n{\n\t"appid"\t\t"2183900"\n\t"name"\t\t"Warhammer 40,000: Space Marine 2"\n'
        f'\t"buildid"\t\t"{buildid}"\n\t"installdir"\t\t"{installdir}"\n}}\n', encoding='utf-8')
    fake_exe(apps / 'common' / installdir / 'client_pc/root/bin/pc' / EXE, timestamp)


def library_folders(steam, *libraries):
    entries = ''.join(
        f'\t"{i}"\n\t{{\n\t\t"path"\t\t"{str(lib).replace(chr(92), chr(92) * 2)}"\n'
        f'\t\t"apps"\n\t\t{{\n\t\t\t"228980"\t\t"1"\n\t\t}}\n\t}}\n'
        for i, lib in enumerate([steam, *libraries]))
    (steam / 'steamapps').mkdir(parents=True, exist_ok=True)
    (steam / 'steamapps/libraryfolders.vdf').write_text(f'"libraryfolders"\n{{\n{entries}}}\n', encoding='utf-8')


class FindGameTests(unittest.TestCase):
    def setUp(self):
        TEMP.mkdir(parents=True, exist_ok=True)
        self.work = Path(tempfile.mkdtemp(dir=TEMP))

    def test_game_in_a_second_library_with_spaces_and_non_ascii(self):
        steam = self.work / 'Steam'
        library = self.work / 'Spiele Bibliothek ü'
        library_folders(steam, library)
        install_game(library)
        game = run_ps_json(FIXTURES / 'find_game.ps1', steam)
        self.assertEqual(Path(game['Root']), library / 'steamapps/common/Space Marine 2')
        self.assertEqual(Path(game['Mods']), library / 'steamapps/common/Space Marine 2/client_pc/root/mods')
        self.assertEqual(game['BuildId'], '26074365')
        self.assertEqual(game['PeTimestamp'], '0x6ad10fcb')

    def test_game_in_the_steam_folder_itself(self):
        steam = self.work / 'Steam'
        library_folders(steam)
        install_game(steam, installdir='SM2')
        game = run_ps_json(FIXTURES / 'find_game.ps1', steam)
        self.assertEqual(Path(game['Exe']), steam / 'steamapps/common/SM2/client_pc/root/bin/pc' / EXE)

    def test_manifest_without_the_exe_is_skipped(self):
        steam = self.work / 'Steam'
        other = self.work / 'Other'
        library_folders(steam, other)
        install_game(steam)
        (steam / 'steamapps/common/Space Marine 2/client_pc/root/bin/pc' / EXE).unlink()
        install_game(other, buildid='1')
        game = run_ps_json(FIXTURES / 'find_game.ps1', steam)
        self.assertEqual(game['BuildId'], '1')

    def test_not_installed(self):
        steam = self.work / 'Steam'
        library_folders(steam)
        result = run_ps(FIXTURES / 'find_game.ps1', steam)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), 'null')

    def test_override_skips_steam_and_must_hold_the_game(self):
        root = self.work / 'Copied Game'
        fake_exe(root / 'client_pc/root/bin/pc' / EXE, 0x12345678)
        game = run_ps_json(FIXTURES / 'find_game.ps1', self.work / 'no steam', '-Override', root)
        self.assertEqual(game['PeTimestamp'], '0x12345678')
        result = run_ps(FIXTURES / 'find_game.ps1', self.work / 'no steam', '-Override', self.work)
        self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()
