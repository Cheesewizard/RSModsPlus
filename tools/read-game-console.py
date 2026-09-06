# Read the Rocksmith debug console's screen buffer without touching the game.
#
# The RSMods logger holds RSMods_debug.txt exclusively for the process lifetime, so the
# log file cannot be read while the game runs. The attached debug console can: attach to
# the game's console, open CONOUT$, and read the character buffer (about 9000 rows of
# scrollback). Read-only; the game never notices.
#
# Usage:
#   python tools/read-game-console.py                   # last 200 rows
#   python tools/read-game-console.py 400               # last 400 rows
#   python tools/read-game-console.py 0 "NBN GRID"      # whole buffer, filtered
import ctypes
import ctypes.wintypes as wt
import subprocess
import sys

k32 = ctypes.windll.kernel32


class COORD(ctypes.Structure):
    _fields_ = [("X", ctypes.c_short), ("Y", ctypes.c_short)]


class SMALL_RECT(ctypes.Structure):
    _fields_ = [("L", ctypes.c_short), ("T", ctypes.c_short),
                ("R", ctypes.c_short), ("B", ctypes.c_short)]


class CSBI(ctypes.Structure):
    _fields_ = [("Size", COORD), ("Cursor", COORD), ("Attr", ctypes.c_ushort),
                ("Win", SMALL_RECT), ("Max", COORD)]


def main():
    tail = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    pattern = sys.argv[2] if len(sys.argv) > 2 else None

    pid = int(subprocess.check_output(
        ["powershell", "-NoProfile", "-Command", "(Get-Process Rocksmith2014).Id"]
    ).decode().strip())

    k32.FreeConsole()
    if not k32.AttachConsole(pid):
        print("AttachConsole failed (error %d)" % ctypes.GetLastError())
        return 1

    handle = k32.CreateFileW("CONOUT$", 0xC0000000, 3, None, 3, 0, None)
    info = CSBI()
    if not k32.GetConsoleScreenBufferInfo(handle, ctypes.byref(info)):
        print("GetConsoleScreenBufferInfo failed (error %d)" % ctypes.GetLastError())
        return 1

    width, cursor = info.Size.X, info.Cursor.Y
    start = 0 if tail == 0 else max(0, cursor - tail)
    buf = ctypes.create_unicode_buffer(width)
    read = wt.DWORD()
    for y in range(start, cursor + 1):
        k32.ReadConsoleOutputCharacterW(handle, buf, width, COORD(0, y), ctypes.byref(read))
        line = buf.value.rstrip()
        if not line:
            continue
        if pattern is None or pattern in line:
            print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
