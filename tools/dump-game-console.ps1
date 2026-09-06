param([int]$GamePid, [string]$OutFile)

Add-Type -Namespace Win32 -Name Console -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool FreeConsole();
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool AttachConsole(uint dwProcessId);
[DllImport("kernel32.dll", SetLastError = true)]
public static extern IntPtr CreateFileW(
    [MarshalAs(UnmanagedType.LPWStr)] string lpFileName,
    uint dwDesiredAccess, uint dwShareMode, IntPtr lpSecurityAttributes,
    uint dwCreationDisposition, uint dwFlagsAndAttributes, IntPtr hTemplateFile);
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool GetConsoleScreenBufferInfo(IntPtr hConsoleOutput, out CONSOLE_SCREEN_BUFFER_INFO lpInfo);
[DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern bool ReadConsoleOutputCharacterW(
    IntPtr hConsoleOutput, [Out] char[] lpCharacter, uint nLength, COORD dwReadCoord, out uint lpNumberOfCharsRead);
[StructLayout(LayoutKind.Sequential)]
public struct COORD { public short X; public short Y; }
[StructLayout(LayoutKind.Sequential)]
public struct SMALL_RECT { public short Left; public short Top; public short Right; public short Bottom; }
[StructLayout(LayoutKind.Sequential)]
public struct CONSOLE_SCREEN_BUFFER_INFO
{
    public COORD dwSize; public COORD dwCursorPosition; public ushort wAttributes;
    public SMALL_RECT srWindow; public COORD dwMaximumWindowSize;
}
'@

$result = New-Object System.Text.StringBuilder
[Win32.Console]::FreeConsole() | Out-Null
if (-not [Win32.Console]::AttachConsole([uint32]$GamePid))
{
    Set-Content -Path $OutFile -Value "ATTACH FAILED: $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error())" -Encoding utf8
    exit 1
}

# GetStdHandle after attach can return a redirected handle; open CONOUT$ directly.
$handle = [Win32.Console]::CreateFileW('CONOUT$', [uint32]"0xC0000000", 0x3, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
$info = New-Object Win32.Console+CONSOLE_SCREEN_BUFFER_INFO
if (-not [Win32.Console]::GetConsoleScreenBufferInfo($handle, [ref]$info))
{
    Set-Content -Path $OutFile -Value "BUFFERINFO FAILED: $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error())" -Encoding utf8
    exit 1
}

$width = $info.dwSize.X
$height = $info.dwSize.Y
$buffer = New-Object char[] $width
for ($row = 0; $row -lt $height; $row++)
{
    $coord = New-Object Win32.Console+COORD
    $coord.X = 0
    $coord.Y = [int16]$row
    $read = [uint32]0
    if ([Win32.Console]::ReadConsoleOutputCharacterW($handle, $buffer, [uint32]$width, $coord, [ref]$read))
    {
        $line = (-join $buffer[0..($read - 1)]).TrimEnd()
        [void]$result.AppendLine($line)
    }
}
Set-Content -Path $OutFile -Value ($result.ToString()) -Encoding utf8
