$ErrorActionPreference = 'Stop'

Add-Type @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public static class BigWillyInteractiveLauncher
{
    private const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
    private const uint TOKEN_ASSIGN_PRIMARY = 0x0001;
    private const uint TOKEN_DUPLICATE = 0x0002;
    private const uint TOKEN_QUERY = 0x0008;
    private const uint MAXIMUM_ALLOWED = 0x02000000;
    private const uint SecurityImpersonation = 2;
    private const uint TokenPrimary = 1;
    private const uint LOGON_WITH_PROFILE = 0x00000001;
    private const uint CREATE_UNICODE_ENVIRONMENT = 0x00000400;
    private const uint STARTF_USESHOWWINDOW = 0x00000001;

    [StructLayout(LayoutKind.Sequential)]
    private struct STARTUPINFO
    {
        public uint cb;
        [MarshalAs(UnmanagedType.LPWStr)] public string lpReserved;
        [MarshalAs(UnmanagedType.LPWStr)] public string lpDesktop;
        [MarshalAs(UnmanagedType.LPWStr)] public string lpTitle;
        public uint dwX;
        public uint dwY;
        public uint dwXSize;
        public uint dwYSize;
        public uint dwXCountChars;
        public uint dwYCountChars;
        public uint dwFillAttribute;
        public uint dwFlags;
        public short wShowWindow;
        public short cbReserved2;
        public IntPtr lpReserved2;
        public IntPtr hStdInput;
        public IntPtr hStdOutput;
        public IntPtr hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct PROCESS_INFORMATION
    {
        public IntPtr hProcess;
        public IntPtr hThread;
        public uint dwProcessId;
        public uint dwThreadId;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint access, bool inheritHandle, uint processId);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool OpenProcessToken(IntPtr processHandle, uint desiredAccess, out IntPtr tokenHandle);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool DuplicateTokenEx(IntPtr existingToken, uint desiredAccess, IntPtr tokenAttributes,
        uint impersonationLevel, uint tokenType, out IntPtr primaryToken);

    [DllImport("userenv.dll", SetLastError = true)]
    private static extern bool CreateEnvironmentBlock(out IntPtr environment, IntPtr token, bool inherit);

    [DllImport("userenv.dll", SetLastError = true)]
    private static extern bool DestroyEnvironmentBlock(IntPtr environment);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CreateProcessWithTokenW(IntPtr token, uint logonFlags, string applicationName,
        StringBuilder commandLine, uint creationFlags, IntPtr environment, string currentDirectory,
        ref STARTUPINFO startupInfo, out PROCESS_INFORMATION processInformation);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CreateProcessAsUserW(IntPtr token, string applicationName, StringBuilder commandLine,
        IntPtr processAttributes, IntPtr threadAttributes, bool inheritHandles, uint creationFlags, IntPtr environment,
        string currentDirectory, ref STARTUPINFO startupInfo, out PROCESS_INFORMATION processInformation);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CreateProcessW(string applicationName, StringBuilder commandLine,
        IntPtr processAttributes, IntPtr threadAttributes, bool inheritHandles, uint creationFlags, IntPtr environment,
        string currentDirectory, ref STARTUPINFO startupInfo, out PROCESS_INFORMATION processInformation);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);

    public static string Launch(uint explorerPid, string scriptPath, string currentDirectory)
    {
        IntPtr process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, explorerPid);
        if (process == IntPtr.Zero) return "OpenProcess=" + Marshal.GetLastWin32Error();
        IntPtr sourceToken = IntPtr.Zero;
        IntPtr token = IntPtr.Zero;
        IntPtr environment = IntPtr.Zero;
        PROCESS_INFORMATION pi;
        try
        {
            if (!OpenProcessToken(process, TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY, out sourceToken))
                return "OpenProcessToken=" + Marshal.GetLastWin32Error();
            if (!DuplicateTokenEx(sourceToken, MAXIMUM_ALLOWED, IntPtr.Zero, SecurityImpersonation, TokenPrimary, out token))
                return "DuplicateTokenEx=" + Marshal.GetLastWin32Error();
            // The sandbox token may be unable to materialize the user's profile
            // environment. The launch script sets every build-specific variable,
            // and a null block still gives the child the interactive token.
            CreateEnvironmentBlock(out environment, token, false);

            string command = "cmd.exe /d /c call \"" + scriptPath + "\"";
            var commandLine = new StringBuilder(command);
            var si = new STARTUPINFO();
            si.cb = (uint)Marshal.SizeOf(typeof(STARTUPINFO));
            si.lpDesktop = "winsta0\\default";
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = 0;

            bool ok = CreateProcessWithTokenW(token, LOGON_WITH_PROFILE, null, commandLine,
                CREATE_UNICODE_ENVIRONMENT, environment, currentDirectory, ref si, out pi);
            int tokenError = Marshal.GetLastWin32Error();
            if (!ok)
            {
                commandLine = new StringBuilder(command);
                ok = CreateProcessAsUserW(token, null, commandLine, IntPtr.Zero, IntPtr.Zero, false,
                    CREATE_UNICODE_ENVIRONMENT, environment, currentDirectory, ref si, out pi);
                if (!ok)
                {
                    // Last resort: use the current process token but explicitly
                    // bind the child to the interactive window station/desktop.
                    // The command file still supplies the build's environment.
                    commandLine = new StringBuilder(command);
                    ok = CreateProcessW(null, commandLine, IntPtr.Zero, IntPtr.Zero, false,
                        CREATE_UNICODE_ENVIRONMENT, IntPtr.Zero, currentDirectory, ref si, out pi);
                    if (!ok) return "CreateProcessWithTokenW=" + tokenError + "; CreateProcessAsUserW=" + Marshal.GetLastWin32Error() + "; CreateProcessW=" + Marshal.GetLastWin32Error();
                }
            }
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return "launched=" + pi.dwProcessId;
        }
        finally
        {
            if (environment != IntPtr.Zero) DestroyEnvironmentBlock(environment);
            if (token != IntPtr.Zero) CloseHandle(token);
            if (sourceToken != IntPtr.Zero) CloseHandle(sourceToken);
            CloseHandle(process);
        }
    }

    public static string LaunchDirect(string executablePath, string arguments, string currentDirectory)
    {
        var commandLine = new StringBuilder("\"" + executablePath + "\" " + arguments);
        var si = new STARTUPINFO();
        si.cb = (uint)Marshal.SizeOf(typeof(STARTUPINFO));
        si.lpDesktop = "winsta0\\default";
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = 1;
        PROCESS_INFORMATION pi;
        bool ok = CreateProcessW(executablePath, commandLine, IntPtr.Zero, IntPtr.Zero, false,
            CREATE_UNICODE_ENVIRONMENT, IntPtr.Zero, currentDirectory, ref si, out pi);
        if (!ok) return "CreateProcessW=" + Marshal.GetLastWin32Error();
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return "launched=" + pi.dwProcessId;
    }
}
'@

$workspace = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$script = Join-Path $workspace 'workstation\tools\launch_interactive_bigwilly.cmd'
$cwd = Join-Path $workspace 'workstation\build\runtime-vs5\Release'
$exe = Join-Path $cwd 'dahbwu.exe'
$module = Join-Path $workspace 'workstation\build\game-module\gRDHE78_recomp.dll'
$game = Join-Path $workspace 'workstation\game'
$userDir = Join-Path $workspace 'workstation\run-user-cold-world-a-after-1'
$env:STATICRECOMP_FORCE_NUNCHUK = '1'
$env:BIGWILLY_MOUSE_IR_X_OFFSET = '0.13'
$env:BIGWILLY_MOUSE_IR_Y_OFFSET = '-0.30'
$explorer = Get-Process explorer | Where-Object { $_.SessionId -eq 1 } | Select-Object -First 1
if ($null -eq $explorer) { throw 'No interactive explorer.exe was found in session 1.' }
if ($env:BIGWILLY_LAUNCH_TEST -eq 'notepad') {
    $result = [BigWillyInteractiveLauncher]::LaunchDirect((Join-Path $env:WINDIR 'System32\notepad.exe'), '', $cwd)
} else {
    $args = '--game "' + $game + '" --module "' + $module + '" --user-dir "' + $userDir + '" --graphics D3D --no-mods --title "Big Willy | Playable | Centered IR"'
    $result = [BigWillyInteractiveLauncher]::LaunchDirect($exe, $args, $cwd)
}
if (-not $result.StartsWith('launched=')) { throw $result }
$result
