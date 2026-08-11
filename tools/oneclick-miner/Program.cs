using System.Diagnostics;
using System.Formats.Tar;
using System.IO.Compression;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace ReSatoshi.OneClickMiner;

internal static class Constants
{
    internal const string Commit = "fa70158bf1a560da666b10ad357800ca7b250428";
    internal const string Genesis = "00003238910a7bd34d9175b5b9929aeb491641d722b5c1c1eaa8aafafc05c55a";
    internal const string Peer = "58.123.125.154:49595";
    internal const int P2pPort = 49595;
    internal const int RpcPort = 49694;
    internal const string Wallet = "resatoshi-miner";
    internal const string Warning = "PUBLIC ALPHA TEST ONLY — Alpha RST has no guaranteed monetary value and the chain may be reset.";
}

internal sealed record EmbeddedFile(string Name, string Sha256, long Size);
internal sealed record EmbeddedManifest(string Commit, EmbeddedFile[] Files);

internal sealed class PayloadManager
{
    private readonly string _root;
    internal string AppDir => Path.Combine(_root, "app");
    internal string DataDir => Path.Combine(_root, "data");
    internal string SourceDir => Path.Combine(_root, "source", "fa70158");
    internal string Daemon => Path.Combine(AppDir, "resatoshid.exe");
    internal string Cli => Path.Combine(AppDir, "resatoshi-cli.exe");

    internal PayloadManager(string root) => _root = root;

    internal async Task ExtractAndVerifyAsync(CancellationToken token = default)
    {
        Directory.CreateDirectory(AppDir);
        Directory.CreateDirectory(DataDir);
        Directory.CreateDirectory(Path.Combine(_root, "source"));

        var assembly = Assembly.GetExecutingAssembly();
        await using var manifestStream = assembly.GetManifestResourceStream("Payload.manifest.json")
            ?? throw new InvalidDataException("Embedded manifest is missing.");
        var manifest = await JsonSerializer.DeserializeAsync<EmbeddedManifest>(manifestStream, cancellationToken: token)
            ?? throw new InvalidDataException("Embedded manifest is invalid.");
        if (!StringComparer.Ordinal.Equals(manifest.Commit, Constants.Commit))
            throw new InvalidDataException("Embedded source commit does not match this application.");

        foreach (var file in manifest.Files)
        {
            token.ThrowIfCancellationRequested();
            var resourceName = "Payload." + file.Name;
            await using var resource = assembly.GetManifestResourceStream(resourceName)
                ?? throw new InvalidDataException($"Embedded file is missing: {file.Name}");
            var target = Path.Combine(AppDir, file.Name);
            if (File.Exists(target) && await HashAsync(target, token) == file.Sha256 && new FileInfo(target).Length == file.Size)
                continue;
            var temporary = target + ".new-" + Guid.NewGuid().ToString("N");
            try
            {
                await using (var output = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                    await resource.CopyToAsync(output, token);
                if (await HashAsync(temporary, token) != file.Sha256 || new FileInfo(temporary).Length != file.Size)
                    throw new InvalidDataException($"SHA-256 verification failed: {file.Name}");
                File.Move(temporary, target, true);
            }
            finally
            {
                if (File.Exists(temporary)) File.Delete(temporary);
            }
        }
        await EnsureSourcesExtractedAsync(token);
    }

    private async Task EnsureSourcesExtractedAsync(CancellationToken token)
    {
        var marker = Path.Combine(SourceDir, ".complete-" + Constants.Commit);
        if (File.Exists(marker)) return;
        var temporary = SourceDir + ".new-" + Guid.NewGuid().ToString("N");
        Directory.CreateDirectory(temporary);
        try
        {
            Directory.CreateDirectory(Path.Combine(temporary, "resatoshi"));
            await using (var input = File.OpenRead(Path.Combine(AppDir, "resatoshi-source-fa70158.tar.gz")))
            await using (var gzip = new GZipStream(input, CompressionMode.Decompress))
                TarFile.ExtractToDirectory(gzip, Path.Combine(temporary, "resatoshi"), overwriteFiles: false);
            ZipFile.ExtractToDirectory(Path.Combine(AppDir, "oneclick-miner-source.zip"), Path.Combine(temporary, "oneclick-miner"));
            await File.WriteAllTextAsync(Path.Combine(temporary, ".complete-" + Constants.Commit), Constants.Commit, token);
            if (Directory.Exists(SourceDir)) Directory.Delete(SourceDir, true);
            Directory.Move(temporary, SourceDir);
        }
        finally
        {
            if (Directory.Exists(temporary)) Directory.Delete(temporary, true);
        }
    }

    internal static async Task<string> HashAsync(string path, CancellationToken token = default)
    {
        await using var stream = File.OpenRead(path);
        var hash = await SHA256.HashDataAsync(stream, token);
        return Convert.ToHexString(hash).ToLowerInvariant();
    }
}

internal sealed record ChainSnapshot(int Peers, long Blocks, long Headers, bool InitialBlockDownload,
    double VerificationProgress, string Genesis, decimal Trusted, decimal Immature);

internal static class MiningGate
{
    internal static bool CanMine(ChainSnapshot state) =>
        state.Peers > 0 &&
        StringComparer.Ordinal.Equals(state.Genesis, Constants.Genesis) &&
        !state.InitialBlockDownload &&
        state.Blocks >= state.Headers &&
        state.VerificationProgress >= 0.999999;
}

internal sealed class NodeController : IAsyncDisposable
{
    private readonly PayloadManager _payload;
    private Process? _daemon;
    private CancellationTokenSource? _miningCts;
    private Task? _miningTask;
    internal long FoundBlocks { get; private set; }
    internal string RewardAddress { get; private set; } = "—";
    internal bool IsRunning => _daemon is { HasExited: false };
    internal event Action<string>? Status;
    internal event Action? Updated;

    internal NodeController(PayloadManager payload) => _payload = payload;

    internal static IReadOnlyList<string> BuildNodeArguments() => new[]
    {
        "-testnet",
        "-datadir=" + Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "ReSatoshi-OneClick-Miner", "data"),
        "-connect=" + Constants.Peer,
        "-port=" + Constants.P2pPort,
        "-rpcport=" + Constants.RpcPort,
        "-rpcbind=127.0.0.1",
        "-rpcallowip=127.0.0.1",
        "-listen=0",
        "-server=1",
        "-daemon=0"
    };

    private IReadOnlyList<string> ActualNodeArguments() => new[]
    {
        "-testnet", "-datadir=" + _payload.DataDir, "-connect=" + Constants.Peer,
        "-port=" + Constants.P2pPort, "-rpcport=" + Constants.RpcPort,
        "-rpcbind=127.0.0.1", "-rpcallowip=127.0.0.1", "-listen=0", "-server=1", "-daemon=0"
    };

    internal async Task StartAsync(CancellationToken token)
    {
        if (IsRunning) return;
        Status?.Invoke("내장 파일 무결성 확인 중…");
        await _payload.ExtractAndVerifyAsync(token);
        Status?.Invoke("공개 알파 노드 시작 중…");
        var psi = new ProcessStartInfo(_payload.Daemon) {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardError = true, RedirectStandardOutput = true,
            WorkingDirectory = _payload.AppDir
        };
        foreach (var arg in ActualNodeArguments()) psi.ArgumentList.Add(arg);
        _daemon = Process.Start(psi) ?? throw new InvalidOperationException("Could not start resatoshid.exe.");
        _ = DrainAsync(_daemon.StandardOutput);
        _ = DrainAsync(_daemon.StandardError);
        await WaitForRpcAsync(token);
        var genesis = (await RpcAsync(token, "getblockhash", "0")).Trim().Trim('"');
        if (!StringComparer.Ordinal.Equals(genesis, Constants.Genesis))
        {
            await StopAsync();
            throw new InvalidDataException($"잘못된 체인입니다. Genesis: {genesis}");
        }
        await EnsureWalletAsync(token);
        Status?.Invoke("노드 연결 및 동기화 확인 중…");
        Updated?.Invoke();
    }

    private static async Task DrainAsync(StreamReader reader)
    {
        try { while (await reader.ReadLineAsync() is not null) { } } catch { }
    }

    private async Task WaitForRpcAsync(CancellationToken token)
    {
        Exception? last = null;
        for (var i = 0; i < 90; i++)
        {
            token.ThrowIfCancellationRequested();
            if (_daemon?.HasExited == true) throw new InvalidOperationException("resatoshid.exe exited during startup.");
            try { await RpcAsync(token, "getblockcount"); return; }
            catch (Exception ex) { last = ex; await Task.Delay(1000, token); }
        }
        throw new TimeoutException("RPC startup timed out.", last);
    }

    private async Task EnsureWalletAsync(CancellationToken token)
    {
        using var dirs = JsonDocument.Parse(await RpcAsync(token, "listwalletdir"));
        var exists = dirs.RootElement.GetProperty("wallets").EnumerateArray()
            .Any(w => w.GetProperty("name").GetString() == Constants.Wallet);
        using var loaded = JsonDocument.Parse(await RpcAsync(token, "listwallets"));
        var isLoaded = loaded.RootElement.EnumerateArray().Any(w => w.GetString() == Constants.Wallet);
        if (!exists)
            await RpcAsync(token, "createwallet", Constants.Wallet);
        else if (!isLoaded)
            await RpcAsync(token, "loadwallet", Constants.Wallet);

        try
        {
            using var labels = JsonDocument.Parse(await WalletRpcAsync(token, "getaddressesbylabel", "oneclick-miner"));
            RewardAddress = labels.RootElement.EnumerateObject().Select(p => p.Name).FirstOrDefault() ?? "—";
        }
        catch { RewardAddress = "—"; }
        if (RewardAddress == "—")
            RewardAddress = (await WalletRpcAsync(token, "getnewaddress", "oneclick-miner", "bech32")).Trim().Trim('"');
    }

    internal async Task<ChainSnapshot> SnapshotAsync(CancellationToken token)
    {
        var genesis = (await RpcAsync(token, "getblockhash", "0")).Trim().Trim('"');
        using var network = JsonDocument.Parse(await RpcAsync(token, "getnetworkinfo"));
        using var chain = JsonDocument.Parse(await RpcAsync(token, "getblockchaininfo"));
        using var balances = JsonDocument.Parse(await WalletRpcAsync(token, "getbalances"));
        var mine = balances.RootElement.GetProperty("mine");
        return new ChainSnapshot(
            network.RootElement.GetProperty("connections").GetInt32(),
            chain.RootElement.GetProperty("blocks").GetInt64(),
            chain.RootElement.GetProperty("headers").GetInt64(),
            chain.RootElement.GetProperty("initialblockdownload").GetBoolean(),
            chain.RootElement.GetProperty("verificationprogress").GetDouble(), genesis,
            mine.GetProperty("trusted").GetDecimal(), mine.GetProperty("immature").GetDecimal());
    }

    internal void BeginMining()
    {
        if (_miningTask is { IsCompleted: false }) return;
        _miningCts = new CancellationTokenSource();
        _miningTask = Task.Run(() => MiningLoopAsync(_miningCts.Token));
    }

    internal async Task StopMiningAsync()
    {
        if (_miningCts is null) return;
        _miningCts.Cancel();
        try { if (_miningTask is not null) await _miningTask.WaitAsync(TimeSpan.FromSeconds(3)); } catch { }
        _miningCts.Dispose();
        _miningCts = null;
        _miningTask = null;
    }

    private async Task MiningLoopAsync(CancellationToken token)
    {
        while (!token.IsCancellationRequested && IsRunning)
        {
            try
            {
                var snapshot = await SnapshotAsync(token);
                if (!MiningGate.CanMine(snapshot))
                {
                    Status?.Invoke(snapshot.Peers == 0 ? "연결 오류: 피어 0명 — 채굴 차단됨" : "동기화 대기 중 — 채굴 차단됨");
                    Updated?.Invoke();
                    await Task.Delay(3000, token);
                    continue;
                }
                Status?.Invoke("채굴 중…");
                using var result = JsonDocument.Parse(await WalletRpcAsync(token, "generatetoaddress", "1", RewardAddress, "250000"));
                FoundBlocks += result.RootElement.GetArrayLength();
                Updated?.Invoke();
            }
            catch (OperationCanceledException) { break; }
            catch (Exception ex)
            {
                Status?.Invoke("채굴 대기: " + ex.Message);
                Updated?.Invoke();
                try { await Task.Delay(3000, token); } catch { break; }
            }
        }
    }

    internal async Task StopAsync()
    {
        await StopMiningAsync();
        if (!IsRunning) return;
        Status?.Invoke("노드 정상 종료 중…");
        try
        {
            using var stopTimeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
            await RpcAsync(stopTimeout.Token, "stop");
        }
        catch { }
        try { await _daemon!.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(20)); }
        catch
        {
            if (!_daemon!.HasExited) _daemon.Kill(entireProcessTree: true);
            try { await _daemon.WaitForExitAsync(); } catch { }
        }
        _daemon?.Dispose();
        _daemon = null;
        Status?.Invoke("정지됨");
        Updated?.Invoke();
    }

    private Task<string> WalletRpcAsync(CancellationToken token, params string[] command) =>
        RunCliAsync(token, new[] { "-rpcwallet=" + Constants.Wallet }.Concat(command));
    private Task<string> RpcAsync(CancellationToken token, params string[] command) => RunCliAsync(token, command);

    private async Task<string> RunCliAsync(CancellationToken token, IEnumerable<string> command)
    {
        var psi = new ProcessStartInfo(_payload.Cli) {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardError = true, RedirectStandardOutput = true,
            WorkingDirectory = _payload.AppDir
        };
        psi.ArgumentList.Add("-testnet");
        psi.ArgumentList.Add("-datadir=" + _payload.DataDir);
        psi.ArgumentList.Add("-rpcconnect=127.0.0.1");
        psi.ArgumentList.Add("-rpcport=" + Constants.RpcPort);
        foreach (var arg in command) psi.ArgumentList.Add(arg);
        using var process = Process.Start(psi) ?? throw new InvalidOperationException("Could not start resatoshi-cli.exe.");
        var outputTask = process.StandardOutput.ReadToEndAsync(token);
        var errorTask = process.StandardError.ReadToEndAsync(token);
        await process.WaitForExitAsync(token);
        var output = await outputTask;
        var error = await errorTask;
        if (process.ExitCode != 0) throw new InvalidOperationException(error.Trim());
        return output;
    }

    public async ValueTask DisposeAsync() => await StopAsync();
}

internal sealed class MainForm : Form
{
    private readonly PayloadManager _payload;
    private readonly NodeController _node;
    private readonly Label _status = ValueLabel();
    private readonly Label _peers = ValueLabel();
    private readonly Label _height = ValueLabel();
    private readonly Label _sync = ValueLabel();
    private readonly Label _address = ValueLabel();
    private readonly Label _trusted = ValueLabel();
    private readonly Label _immature = ValueLabel();
    private readonly Label _found = ValueLabel();
    private readonly Button _startStop = new() { Text = "채굴 정지", AutoSize = true };
    private readonly System.Windows.Forms.Timer _timer = new() { Interval = 2000 };
    private bool _closing;
    private bool _busy;

    internal MainForm(PayloadManager payload)
    {
        _payload = payload;
        _node = new NodeController(payload);
        _node.Status += s => BeginInvoke(() => _status.Text = s);
        _node.Updated += () => BeginInvoke(UpdateFound);
        Text = "ReSatoshi Public Alpha One-Click Miner";
        Width = 780; Height = 520; MinimumSize = new Size(680, 470); StartPosition = FormStartPosition.CenterScreen;
        var warning = new Label { Text = Constants.Warning, ForeColor = Color.DarkRed, Font = new Font(Font, FontStyle.Bold), AutoSize = true, Padding = new Padding(8) };
        var grid = new TableLayoutPanel { Dock = DockStyle.Fill, AutoScroll = true, ColumnCount = 2, RowCount = 10, Padding = new Padding(16) };
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 190)); grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        grid.Controls.Add(warning, 0, 0); grid.SetColumnSpan(warning, 2);
        AddRow(grid, 1, "현재 상태", _status); AddRow(grid, 2, "연결된 피어 수", _peers);
        AddRow(grid, 3, "현재 블록 높이", _height); AddRow(grid, 4, "동기화 상태", _sync);
        AddRow(grid, 5, "채굴 보상 주소", _address); AddRow(grid, 6, "성숙 잔액", _trusted);
        AddRow(grid, 7, "미성숙 잔액", _immature); AddRow(grid, 8, "이번 실행 발견 블록", _found);
        var buttons = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true };
        var openData = new Button { Text = "데이터 폴더 열기", AutoSize = true };
        var openSource = new Button { Text = "내장 소스코드 열기", AutoSize = true };
        buttons.Controls.AddRange(new Control[] { _startStop, openData, openSource }); grid.Controls.Add(buttons, 0, 9); grid.SetColumnSpan(buttons, 2);
        Controls.Add(grid);
        _startStop.Click += async (_, _) => await ToggleAsync();
        openData.Click += (_, _) => OpenFolder(_payload.DataDir);
        openSource.Click += (_, _) => OpenFolder(_payload.SourceDir);
        Shown += async (_, _) => { await StartAllAsync(); _timer.Start(); };
        _timer.Tick += async (_, _) => await RefreshAsync();
        FormClosing += OnClosing;
        _status.Text = "시작 준비 중…"; UpdateFound();
    }

    private static Label ValueLabel() => new() { Text = "—", AutoSize = true, MaximumSize = new Size(520, 0) };
    private static void AddRow(TableLayoutPanel grid, int row, string name, Control value)
    {
        grid.Controls.Add(new Label { Text = name, Font = new Font(SystemFonts.DefaultFont, FontStyle.Bold), AutoSize = true }, 0, row);
        grid.Controls.Add(value, 1, row);
    }
    private static void OpenFolder(string path) { Directory.CreateDirectory(path); Process.Start(new ProcessStartInfo("explorer.exe", path) { UseShellExecute = true }); }
    private void UpdateFound() { _found.Text = _node.FoundBlocks.ToString(); _address.Text = _node.RewardAddress; }

    private async Task StartAllAsync()
    {
        if (_busy) return; _busy = true; _startStop.Enabled = false;
        try { await _node.StartAsync(CancellationToken.None); _node.BeginMining(); _startStop.Text = "채굴 및 노드 정지"; }
        catch (Exception ex) { _status.Text = "오류: " + ex.Message; MessageBox.Show(this, ex.Message, "ReSatoshi 시작 오류", MessageBoxButtons.OK, MessageBoxIcon.Error); }
        finally { _busy = false; _startStop.Enabled = true; }
    }

    private async Task ToggleAsync()
    {
        if (_busy) return; _busy = true; _startStop.Enabled = false;
        try
        {
            if (_node.IsRunning) { await _node.StopAsync(); _startStop.Text = "채굴 시작"; }
            else await StartAllAsyncCore();
        }
        catch (Exception ex)
        {
            _status.Text = "오류: " + ex.Message;
            MessageBox.Show(this, ex.Message, "ReSatoshi 오류", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
        finally { _busy = false; _startStop.Enabled = true; }
    }
    private async Task StartAllAsyncCore() { await _node.StartAsync(CancellationToken.None); _node.BeginMining(); _startStop.Text = "채굴 및 노드 정지"; }

    private async Task RefreshAsync()
    {
        if (_busy || !_node.IsRunning) return;
        try
        {
            var s = await _node.SnapshotAsync(CancellationToken.None);
            _peers.Text = s.Peers.ToString(); _height.Text = s.Blocks.ToString();
            _sync.Text = MiningGate.CanMine(s) ? "완료" : $"대기 중 ({s.Blocks}/{s.Headers}, {s.VerificationProgress:P2})";
            _trusted.Text = s.Trusted.ToString("0.00000000"); _immature.Text = s.Immature.ToString("0.00000000"); UpdateFound();
        }
        catch (Exception ex) { _status.Text = "상태 확인 오류: " + ex.Message; }
    }

    private async void OnClosing(object? sender, FormClosingEventArgs e)
    {
        if (_closing) return;
        e.Cancel = true; _closing = true; _timer.Stop(); Enabled = false;
        await _node.StopAsync();
        FormClosing -= OnClosing; Close();
    }
}

internal static class SelfTest
{
    internal static async Task<int> RunAsync()
    {
        var failures = new List<string>();
        var root = Path.Combine(Path.GetTempPath(), "ReSatoshi-OneClick-SelfTest-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            var payload = new PayloadManager(root);
            await Check("embedded extraction and SHA-256", failures, () => payload.ExtractAndVerifyAsync());
            await Check("resatoshid.exe -version", failures, () => RunVersionAsync(payload.Daemon));
            await Check("resatoshi-cli.exe -version", failures, () => RunVersionAsync(payload.Cli));
            await Check("genesis constant in fa70158 source", failures, async () =>
            {
                var text = await File.ReadAllTextAsync(Path.Combine(payload.SourceDir, "resatoshi", "src", "kernel", "chainparams.cpp"));
                if (!text.Contains(Constants.Genesis, StringComparison.Ordinal)) throw new InvalidDataException("Genesis constant not found.");
            });
            await Check("RPC loopback arguments", failures, () =>
            {
                var args = NodeController.BuildNodeArguments();
                if (!args.Contains("-rpcbind=127.0.0.1") || !args.Contains("-rpcallowip=127.0.0.1") ||
                    args.Any(a => a.Contains((Constants.P2pPort - 1).ToString(), StringComparison.Ordinal)) || !args.Contains("-listen=0"))
                    throw new InvalidDataException("Unsafe RPC/listen arguments.");
                return Task.CompletedTask;
            });
            await Check("wrong-chain and zero-peer mining block", failures, () =>
            {
                var good = new ChainSnapshot(1, 5, 5, false, 1, Constants.Genesis, 0, 0);
                if (!MiningGate.CanMine(good) || MiningGate.CanMine(good with { Peers = 0 }) || MiningGate.CanMine(good with { Genesis = "00" }) || MiningGate.CanMine(good with { InitialBlockDownload = true }))
                    throw new InvalidDataException("Mining gate failed.");
                return Task.CompletedTask;
            });
            await Check("local RPC bind and graceful node shutdown", failures, () => TestNodeLifecycleAsync(payload, root));
        }
        finally { try { Directory.Delete(root, true); } catch { } }
        Console.WriteLine(failures.Count == 0 ? "SELF-TEST PASSED" : "SELF-TEST FAILED");
        foreach (var failure in failures) Console.WriteLine("FAIL: " + failure);
        return failures.Count == 0 ? 0 : 1;
    }

    private static async Task Check(string name, List<string> failures, Func<Task> test)
    {
        try { await test(); Console.WriteLine("PASS: " + name); }
        catch (Exception ex) { failures.Add(name + ": " + ex.Message); Console.WriteLine("FAIL: " + name + ": " + ex.Message); }
    }
    private static async Task RunVersionAsync(string executable)
    {
        var result = await RunProcessAsync(executable, new[] { "-version" }, TimeSpan.FromSeconds(30));
        if (result.ExitCode != 0 || !result.Output.Contains("ReSatoshi", StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException(result.Output);
    }
    private static async Task TestNodeLifecycleAsync(PayloadManager payload, string root)
    {
        var data = Path.Combine(root, "lifecycle-data"); Directory.CreateDirectory(data);
        var rpcPort = FreePort(); var p2pPort = FreePort();
        var args = new[] { "-regtest", "-datadir=" + data, "-listen=0", "-server=1", "-rpcbind=127.0.0.1", "-rpcallowip=127.0.0.1", "-rpcport=" + rpcPort, "-port=" + p2pPort, "-daemon=0" };
        var psi = new ProcessStartInfo(payload.Daemon) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (var arg in args) psi.ArgumentList.Add(arg);
        using var daemon = Process.Start(psi) ?? throw new InvalidOperationException("Lifecycle daemon did not start.");
        _ = daemon.StandardOutput.ReadToEndAsync(); _ = daemon.StandardError.ReadToEndAsync();
        try
        {
            var ready = false;
            for (var i = 0; i < 30 && !daemon.HasExited; i++)
            {
                var result = await RunProcessAsync(payload.Cli, new[] { "-regtest", "-datadir=" + data, "-rpcconnect=127.0.0.1", "-rpcport=" + rpcPort, "getblockcount" }, TimeSpan.FromSeconds(5), throwOnTimeout: false);
                if (result.ExitCode == 0) { ready = true; break; }
                await Task.Delay(500);
            }
            if (!ready) throw new TimeoutException("Temporary node RPC did not start.");
            var listeners = IPGlobalProperties.GetIPGlobalProperties().GetActiveTcpListeners().Where(e => e.Port == rpcPort).ToArray();
            if (listeners.Length == 0 || listeners.Any(e => !IPAddress.IsLoopback(e.Address))) throw new InvalidDataException("RPC is not loopback-only.");
            var stop = await RunProcessAsync(payload.Cli, new[] { "-regtest", "-datadir=" + data, "-rpcconnect=127.0.0.1", "-rpcport=" + rpcPort, "stop" }, TimeSpan.FromSeconds(10));
            if (stop.ExitCode != 0) throw new InvalidDataException("CLI stop failed: " + stop.Output);
            await daemon.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(20));
        }
        finally { if (!daemon.HasExited) daemon.Kill(entireProcessTree: true); }
    }
    private static int FreePort()
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        try { listener.Start(); return ((IPEndPoint)listener.LocalEndpoint).Port; }
        finally { listener.Stop(); }
    }
    private static async Task<(int ExitCode, string Output)> RunProcessAsync(string file, IEnumerable<string> args, TimeSpan timeout, bool throwOnTimeout = true)
    {
        var psi = new ProcessStartInfo(file) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (var arg in args) psi.ArgumentList.Add(arg);
        using var process = Process.Start(psi) ?? throw new InvalidOperationException("Process did not start: " + file);
        var stdout = process.StandardOutput.ReadToEndAsync(); var stderr = process.StandardError.ReadToEndAsync();
        try { await process.WaitForExitAsync().WaitAsync(timeout); }
        catch (TimeoutException) { if (!process.HasExited) process.Kill(true); if (throwOnTimeout) throw; return (-1, "timeout"); }
        return (process.ExitCode, (await stdout) + (await stderr));
    }
}

internal static class Program
{
    private const int AttachParentProcess = -1;
    [DllImport("kernel32.dll")] private static extern bool AttachConsole(int processId);

    [STAThread]
    private static async Task<int> Main(string[] args)
    {
        if (args.Contains("--self-test", StringComparer.OrdinalIgnoreCase))
        {
            AttachConsole(AttachParentProcess);
            Console.SetOut(new StreamWriter(Console.OpenStandardOutput()) { AutoFlush = true });
            Console.SetError(new StreamWriter(Console.OpenStandardError()) { AutoFlush = true });
            return await SelfTest.RunAsync();
        }
        using var mutex = new Mutex(true, "Local\\ReSatoshi-OneClick-Miner-fa70158", out var created);
        if (!created) { MessageBox.Show("ReSatoshi One-Click Miner가 이미 실행 중입니다."); return 2; }
        ApplicationConfiguration.Initialize();
        var root = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "ReSatoshi-OneClick-Miner");
        Application.Run(new MainForm(new PayloadManager(root)));
        return 0;
    }
}
