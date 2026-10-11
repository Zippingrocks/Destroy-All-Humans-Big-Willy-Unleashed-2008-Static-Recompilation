using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

internal static class Program
{
    [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int MessageBoxW(IntPtr owner, string text, string caption, uint type);

    const int SW_RESTORE = 9;
    const uint MB_ICONERROR = 0x10;
    const uint MB_ICONINFO  = 0x40;

    static readonly string ConfigFile =
        Path.Combine(AppContext.BaseDirectory, "bigwilly.conf");

    static StreamWriter? s_furonlog;
    static readonly object FuronLock = new();
    static string startupStatus = "Preparing for invasion…";

    static void FuronWrite(string tag, string? line)
    {
        if (line is null) return;
        if (line.Contains("Checking extracted game files"))
            Volatile.Write(ref startupStatus, "Checking game files…");
        else if (line.Contains("Creating game runtime"))
            Volatile.Write(ref startupStatus, "Preparing the invasion…");
        else if (line.Contains("preloading extracted game assets"))
            Volatile.Write(ref startupStatus, "Loading the world…");
        else if (line.Contains("Opening game window"))
            Volatile.Write(ref startupStatus, "Launching Big Willy Unleashed…");
        lock (FuronLock)
        {
            try { s_furonlog?.WriteLine($"[{DateTime.Now:HH:mm:ss.fff}][{tag}] {line}"); }
            catch { }
        }
    }

    [STAThread]
    static void Main(string[] args)
    {
        Application.SetHighDpiMode(HighDpiMode.SystemAware);

        // Render the real splash for visual QA without starting or controlling a game.
        if (args.Length == 2 && args[0] == "--render-splash")
        {
            using var splash = new GameSplash();
            splash.SetStatus("Loading the world…");
            splash.CreateControl();
            using var bitmap = new System.Drawing.Bitmap(splash.Width, splash.Height);
            splash.DrawToBitmap(bitmap, splash.ClientRectangle);
            bitmap.Save(Path.GetFullPath(args[1]), System.Drawing.Imaging.ImageFormat.Png);
            return;
        }

        string? workspace = FindWorkspace();

        if (workspace is null)
        {
            MessageBoxW(IntPtr.Zero,
                "Could not find the Big Willy workspace automatically.\n\n" +
                "Click OK, then browse to dahbwu.exe.",
                "Big Willy Launcher", MB_ICONINFO);

            string? exePath = BrowseForDahbwu();
            if (exePath is null) return;

            workspace = ResolveWorkspaceFromExe(exePath);
            if (workspace is null)
            {
                MessageBoxW(IntPtr.Zero,
                    $"Could not determine workspace root from:\n{exePath}\n\n" +
                    "Expected to find workstation\\game next to or above the build folder.",
                    "Big Willy Launcher", MB_ICONERROR);
                return;
            }

            SaveConfig("workspace", workspace);
        }

        try { Launch(workspace, args.Contains("--resume-gameplay", StringComparer.OrdinalIgnoreCase)); }
        catch (Exception ex)
        {
            MessageBoxW(IntPtr.Zero, $"Launch failed: {ex.Message}", "Big Willy Launcher", MB_ICONERROR);
        }
    }

    // ── Workspace discovery ──────────────────────────────────────────────────

    static string? FindWorkspace()
    {
        // 1. Config file written from a previous browse
        string? saved = LoadConfig("workspace");
        if (saved is not null && IsWorkspaceRoot(saved)) return saved;

        // 2. Walk up from the exe's own directory
        string? dir = AppContext.BaseDirectory;
        while (dir is not null)
        {
            if (IsWorkspaceRoot(dir)) return dir;
            dir = Path.GetDirectoryName(dir);
        }

        // 3. Hardcoded original location as last-resort fallback
        const string original =
            @"D:\.codex\.codex workspaces\Destroy All Humans! Big Willy Unleashed Recompiled";
        if (IsWorkspaceRoot(original)) return original;

        return null;
    }

    // Walk upward from wherever dahbwu.exe lives until we find the workspace root.
    static string? ResolveWorkspaceFromExe(string exePath)
    {
        string? dir = Path.GetDirectoryName(exePath);
        while (dir is not null)
        {
            if (IsWorkspaceRoot(dir)) return dir;
            dir = Path.GetDirectoryName(dir);
        }
        return null;
    }

    // A directory is the workspace root when it has both the runtime exe and the game data.
    static bool IsWorkspaceRoot(string dir) =>
        File.Exists(Path.Combine(dir, "dist", "flagship", "dahbwu.exe")) &&
        Directory.Exists(Path.Combine(dir, "workstation", "game"));

    // ── Launch logic ─────────────────────────────────────────────────────────

    static void Launch(string workspace, bool resumeGameplay = false)
    {
        var logPath    = Path.Combine(workspace, "workstation", "logs", "interactive-launch.log");
        var furonPath  = Path.Combine(workspace, "workstation", "logs", "bigwilly.furonlog");
        var autoDir    = Path.Combine(workspace, "workstation", "interactive-automation");
        var runtimeDir = Path.Combine(workspace, "dist", "flagship");
        var executable = Path.Combine(runtimeDir, "dahbwu.exe");
        var game       = Path.Combine(workspace, "workstation", "game");
        var module     = Path.Combine(runtimeDir, "gRDHE78_recomp.dll");
        var userDir    = Path.Combine(workspace, "workstation", "run-user");

        // Validate every required piece before attempting launch.
        var missing = new List<string>();
        if (!File.Exists(executable)) missing.Add($"Runtime:     {executable}");
        if (!Directory.Exists(game))  missing.Add($"Game data:   {game}");
        if (!File.Exists(module))     missing.Add($"Game module: {module}");
        if (!Directory.Exists(Path.Combine(runtimeDir, "Sys"))) missing.Add("Runtime Sys directory");
        if (!File.Exists(Path.Combine(game, "sys", "main.dol"))) missing.Add("Extracted game sys/main.dol");

        if (missing.Count > 0)
        {
            MessageBoxW(IntPtr.Zero,
                "Cannot launch — missing required files:\n\n" +
                string.Join("\n", missing) + "\n\n" +
                "Build the project first (workstation\\build-game.cmd + build-runtime.cmd).",
                "Big Willy Launcher", MB_ICONERROR);
            return;
        }

        Directory.CreateDirectory(Path.GetDirectoryName(logPath)!);
        Directory.CreateDirectory(userDir);
        Directory.CreateDirectory(autoDir);
        var controllerConfig = Path.Combine(userDir, "Config", "WiimoteNew.ini");
        if (!File.Exists(controllerConfig) || new FileInfo(controllerConfig).Length == 0)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(controllerConfig)!);
            File.Copy(Path.Combine(runtimeDir, "WiimoteNew.ini"), controllerConfig, true);
        }
        var profileText = File.ReadAllText(controllerConfig);
        if (profileText.Contains("Device = DInput/0/Keyboard Mouse") && profileText.Contains("Buttons/2 = E"))
            File.WriteAllText(controllerConfig, profileText.Replace("Buttons/2 = E", "Buttons/2 = R"));
        foreach (var sub in new[] { "commands", "processed", "failed", "artifacts" })
            Directory.CreateDirectory(Path.Combine(autoDir, sub));

        File.AppendAllText(logPath,
            $"==== {DateTime.Now:yyyy-MM-dd HH:mm:ss} gui-launcher" +
            $" user={Environment.UserDomainName}\\{Environment.UserName}" +
            $" session={Environment.GetEnvironmentVariable("SESSIONNAME")}" +
            $" workspace={workspace} ====\r\n");

        // Bring an existing instance to the front instead of launching a second one.
        foreach (var existing in Process.GetProcessesByName("dahbwu"))
        {
            try
            {
                if (existing.SessionId != Process.GetCurrentProcess().SessionId ||
                    !string.Equals(existing.MainModule?.FileName, executable, StringComparison.OrdinalIgnoreCase))
                    continue;
                if (existing.MainWindowHandle != IntPtr.Zero)
                {
                    ShowWindow(existing.MainWindowHandle, SW_RESTORE);
                    SetForegroundWindow(existing.MainWindowHandle);
                }
                File.AppendAllText(logPath, $"existing-pid={existing.Id}\r\n");
                if (existing.MainWindowHandle == IntPtr.Zero)
                    MessageBoxW(IntPtr.Zero, "Big Willy is already starting. If its window does not appear, see workstation/logs/bigwilly.furonlog.", "Big Willy Launcher", MB_ICONINFO);
                return;
            }
            finally { existing.Dispose(); }
        }

        var arguments =
            $"--game \"{game}\" --module \"{module}\" " +
            $"--user-dir \"{userDir}\" " +
            $"--automation-dir \"{autoDir}\" " +
            $"--graphics D3D --no-mods " +
            $"--title \"Destroy All Humans! Big Willy Unleashed\"";
        if (resumeGameplay)
        {
            var checkpoint = Path.Combine(autoDir, "performance-baseline.sav");
            if (!File.Exists(checkpoint))
                throw new FileNotFoundException("The gameplay checkpoint is missing.", checkpoint);
            arguments += $" --load-state \"{checkpoint}\"";
        }

        s_furonlog = new StreamWriter(furonPath, append: false, Encoding.UTF8) { AutoFlush = true };
        s_furonlog.WriteLine(new string('=', 80));
        s_furonlog.WriteLine("FURON LOG SESSION START");
        s_furonlog.WriteLine($"Date : {DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}");
        s_furonlog.WriteLine($"User : {Environment.UserDomainName}\\{Environment.UserName}");
        s_furonlog.WriteLine($"EXE  : {executable}");
        s_furonlog.WriteLine($"ARGS : {arguments}");
        s_furonlog.WriteLine($"AUTO : {autoDir}");
        s_furonlog.WriteLine(new string('=', 80));

        var psi = new ProcessStartInfo
        {
            FileName               = executable,
            Arguments              = arguments,
            WorkingDirectory       = runtimeDir,
            UseShellExecute        = false,
            CreateNoWindow         = true,
            RedirectStandardOutput = true,
            RedirectStandardError  = true,
        };

        // Interactive launches must not inherit experimental automation or timing switches.
        foreach (var key in psi.Environment.Keys.ToArray())
            if (key.StartsWith("STATICRECOMP_", StringComparison.OrdinalIgnoreCase) ||
                key.StartsWith("BIGWILLY_", StringComparison.OrdinalIgnoreCase))
                psi.Environment.Remove(key);
        psi.EnvironmentVariables["STATICRECOMP_FORCE_NUNCHUK"]  = "1";
        // Omit per-dispatch diagnostic counters during normal play.
        psi.EnvironmentVariables["STATICRECOMP_FAST_STATS"]     = "1";
        psi.EnvironmentVariables["BIGWILLY_PC_CONTROLS"]        = "1";
        // Cache disc assets in host RAM and extend the active map's visibility.
        // Either feature can be disabled independently in bigwilly.conf.
        psi.EnvironmentVariables["BIGWILLY_PRELOAD_ASSETS"] = LoadConfig("preload_assets") == "0" ? "0" : "1";
        psi.EnvironmentVariables["BIGWILLY_FULL_WORLD"] = LoadConfig("full_world") == "0" ? "0" : "1";
        psi.EnvironmentVariables["BIGWILLY_MAX_DETAIL"] = LoadConfig("max_detail") == "1" ? "1" : "0";
        // Validated on the local 7950X3D: retain the native code in its shared L3.
        psi.EnvironmentVariables["MODERNGEKKO_CACHE_AFFINITY"]   = "1";
        // --no-mods permits omitting empty native interception callbacks.
        psi.EnvironmentVariables["STATICRECOMP_SKIP_EMPTY_MOD_CALLS"] = "1";
        // Keep code invalidation, omit data-only invalidation when caches are disabled.
        psi.EnvironmentVariables["BIGWILLY_SKIP_NONCODE_CACHE"]  = "1";
        psi.EnvironmentVariables["BIGWILLY_MOUSE_IR_X_OFFSET"]   = "0.0";
        psi.EnvironmentVariables["BIGWILLY_MOUSE_IR_Y_OFFSET"]   = "0.0";

        var proc = new Process { StartInfo = psi };
        proc.OutputDataReceived += (_, e) => FuronWrite("OUT", e.Data);
        proc.ErrorDataReceived  += (_, e) => FuronWrite("ERR", e.Data);

        try
        {
            proc.Start();
        }
        catch (Exception ex)
        {
            lock (FuronLock) { s_furonlog?.WriteLine($"[SYS] START FAILED: {ex}"); }
            MessageBoxW(IntPtr.Zero,
                $"Failed to start dahbwu.exe:\n{ex.Message}",
                "Big Willy Launcher", MB_ICONERROR);
            return;
        }

        File.AppendAllText(logPath, $"launched-pid={proc.Id}\r\n");
        FuronWrite("SYS", $"launched pid={proc.Id}");

        proc.BeginOutputReadLine();
        proc.BeginErrorReadLine();

        // Startup can spend time checking assets before the game creates a window.
        // Keep a visible, responsive indication rather than silently waiting.
        using (var loading = new GameSplash())
        {
            loading.Show();
            var startupTimer = Stopwatch.StartNew();
            while (!proc.HasExited)
            {
                loading.SetStatus(Volatile.Read(ref startupStatus));
                proc.Refresh();
                if (proc.MainWindowHandle != IntPtr.Zero)
                {
                    ShowWindow(proc.MainWindowHandle, SW_RESTORE);
                    SetForegroundWindow(proc.MainWindowHandle);
                    FuronWrite("SYS", "Game window created");
                    break;
                }
                Application.DoEvents();
                Thread.Sleep(100);
                if (startupTimer.Elapsed > TimeSpan.FromMinutes(5))
                {
                    FuronWrite("SYS", "Startup timeout: no game window after five minutes");
                    MessageBoxW(IntPtr.Zero, $"The game has not created a window after five minutes.\nStartup log: {furonPath}", "Big Willy startup stalled", MB_ICONERROR);
                    break;
                }
            }
            loading.Close();
        }
        proc.WaitForExit();

        lock (FuronLock)
        {
            s_furonlog.WriteLine(new string('=', 80));
            s_furonlog.WriteLine(
                $"FURON LOG SESSION END | exit_code={proc.ExitCode} | {DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}");
            s_furonlog.WriteLine(new string('=', 80));
            s_furonlog.Flush();
            s_furonlog.Dispose();
            s_furonlog = null;
        }

        File.AppendAllText(logPath, $"exit-code={proc.ExitCode}\r\n");

        if (proc.ExitCode != 0)
        {
            MessageBoxW(IntPtr.Zero,
                $"Big Willy exited with code {proc.ExitCode}.\n\nFuronlog:\n{furonPath}",
                "Big Willy Launcher", MB_ICONERROR);
        }
    }

    // ── File browser (WinForms OpenFileDialog) ───────────────────────────────

    static string? BrowseForDahbwu()
    {
        using var dlg = new OpenFileDialog
        {
            Title    = "Locate dahbwu.exe",
            Filter   = "dahbwu.exe|dahbwu.exe|Executables (*.exe)|*.exe",
            FileName = "dahbwu.exe",
        };
        return dlg.ShowDialog() == DialogResult.OK ? dlg.FileName : null;
    }

    // ── Config file (key=value, lives next to the launcher exe) ─────────────

    static string? LoadConfig(string key)
    {
        if (!File.Exists(ConfigFile)) return null;
        foreach (var line in File.ReadAllLines(ConfigFile))
        {
            int eq = line.IndexOf('=');
            if (eq > 0 && line[..eq].Trim() == key)
                return line[(eq + 1)..].Trim();
        }
        return null;
    }

    static void SaveConfig(string key, string value)
    {
        var lines = File.Exists(ConfigFile)
            ? File.ReadAllLines(ConfigFile).ToList()
            : new List<string>();
        int idx = lines.FindIndex(l => l.StartsWith(key + "=") || l.StartsWith(key + " ="));
        if (idx >= 0) lines[idx] = $"{key}={value}";
        else          lines.Add($"{key}={value}");
        File.WriteAllLines(ConfigFile, lines);
    }
}
