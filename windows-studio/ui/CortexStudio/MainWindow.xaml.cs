using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Collections.Generic;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Imaging;
using System.Windows.Shell;
using System.Windows.Threading;
using Microsoft.Win32;

namespace CortexStudio;

public partial class MainWindow : Window

{
    private readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromMilliseconds(200) };

    private bool _initialized, _closing, _closed, _installed, _nativeErrorShown;

    private bool _focusPreview;
    private GridLength _sourceWidth;
    private GridLength _inspectorWidth;
    private Point _dragOrigin;
    private double _startX, _startY;

    private void BeginFraming(object sender, MouseButtonEventArgs e)
    {
        if (BackgroundThumbnail.Source is null) return;
        _dragOrigin = e.GetPosition(BackgroundThumbnail);
        _startX = BackgroundX.Value;
        _startY = BackgroundY.Value;
        BackgroundThumbnail.CaptureMouse();
        e.Handled = true;
    }

    private void DragFraming(object sender, MouseEventArgs e)
    {
        if (!BackgroundThumbnail.IsMouseCaptured) return;
        var delta = e.GetPosition(BackgroundThumbnail) - _dragOrigin;
        BackgroundX.Value = Math.Clamp(_startX - delta.X / Math.Max(1, BackgroundThumbnail.ActualWidth), 0, 1);
        BackgroundY.Value = Math.Clamp(_startY - delta.Y / Math.Max(1, BackgroundThumbnail.ActualHeight), 0, 1);
    }

    private void EndFraming(object sender, MouseButtonEventArgs e) => BackgroundThumbnail.ReleaseMouseCapture();

    private void ZoomFraming(object sender, MouseWheelEventArgs e)
    {
        BackgroundZoom.Value = Math.Clamp(BackgroundZoom.Value + e.Delta / 1200.0, BackgroundZoom.Minimum, BackgroundZoom.Maximum);
        e.Handled = true;
    }

    private int _displayMode = -1, _displayFps = -1;
    private int _avatarMode = 0;
    private string _avatarModelPath = "";
    private bool _loadingAvatarChoices;

    private NativeState _state;

    private string _model = Path.Combine(AppContext.BaseDirectory, "models", "rvm_mobilenetv3_fp32.onnx");

    private string _initialBackground = "";

    private int _initialBackgroundKind, _initialMode, _initialFps = 30, _videoPort = 5004, _controlPort = 5005;

    private bool _initialOutput = true, _initialTransparent;

    private Process? _installer, _networkSetup;

    private readonly string[] _addresses;

    public MainWindow()

    {
        InitializeComponent();

        var area = SystemParameters.WorkArea;

        MinWidth = Math.Min(1080, area.Width - 24);
        MinHeight = Math.Min(720, area.Height - 24);

        Width = Math.Min(1440, area.Width - 48);
        Height = Math.Min(900, area.Height - 48);

        _addresses = Addresses();

        PcAddressBadge.Text = _addresses.Length > 0 ? "PC  " + _addresses[0] : "Connect this PC to your local network";

        ParseArguments();

        PreviewHost.ModelPath = File.Exists(_model) ? _model : "";
        PreviewHost.Visibility = Visibility.Hidden;

        PreviewHost.EngineCreated += OnEngineCreated;

        PreviewHost.EngineError += ShowError;

        BackgroundCard.AllowDrop = true;
        BackgroundCard.Drop += BackgroundCard_Drop;
        BackgroundCard.DragOver += (s, e) =>
        {
            if (e.Data.GetDataPresent(DataFormats.FileDrop))
                e.Effects = DragDropEffects.Copy;
            else
                e.Effects = DragDropEffects.None;
            e.Handled = true;
        };

        _timer.Tick += Poll;

        LoadSavedBackgrounds();

        _initialized = true;
    }

    private static string[] Addresses()

    {
        try
        {
            return NetworkInterface.GetAllNetworkInterfaces()

            .Where(n => n.OperationalStatus == OperationalStatus.Up && n.NetworkInterfaceType is NetworkInterfaceType.Ethernet or NetworkInterfaceType.Wireless80211)

            .SelectMany(n => n.GetIPProperties().UnicastAddresses)

            .Where(a => a.Address.AddressFamily == AddressFamily.InterNetwork && !a.Address.ToString().StartsWith("169.254."))

            .Select(a => a.Address.ToString()).Distinct().Take(3).ToArray();
        }

        catch { return []; }
    }

    private void ParseArguments()

    {
        var args = Environment.GetCommandLineArgs().Skip(1).ToArray();

        for (var i = 0; i < args.Length; i++)

        {
            string Next() => ++i < args.Length ? args[i] : "";

            switch (args[i])

            {
                case "--filter": _initialMode = Next() switch { "chroma" => 1, "ai" => 2, "media" or "background" => 3, _ => 0 }; break;

                case "--fps": _initialFps = Next() == "60" ? 60 : 30; break;

                case "--model": _model = Path.GetFullPath(Next()); break;

                case "--background-image": _initialBackground = Path.GetFullPath(Next()); _initialBackgroundKind = 1; break;

                case "--background-video": _initialBackground = Path.GetFullPath(Next()); _initialBackgroundKind = 2; break;

                case "--no-output": _initialOutput = false; break;

                case "--transparent": _initialTransparent = true; break;

                case "--video-port": if (int.TryParse(Next(), out var vp) && vp is > 0 and <= 65535) _videoPort = vp; break;

                case "--control-port": if (int.TryParse(Next(), out var cp) && cp is > 0 and <= 65535) _controlPort = cp; break;
            }
        }
    }

    private void OnEngineCreated()

    {
        PreviewHost.Visibility = Visibility.Hidden;

        PreviewHost.Set(12, _videoPort + 65536.0 * _controlPort);

        PreviewHost.Set(4, _initialFps);

        PreviewHost.Set(2, _initialMode);

        TransparentToggle.IsChecked = _initialTransparent;

        if (_initialBackground.Length > 0)

        {
            PreviewHost.SetPath(_initialBackgroundKind, _initialBackground);

            UpdateBackgroundCard(_initialBackground, _initialBackgroundKind == 2);
        }

        CheckInstallation();

        OutputToggle.IsChecked = _installed && _initialOutput;

        SelectMode(_initialMode);
        SelectFps(_initialFps);

        _timer.Start();
        Poll(null, EventArgs.Empty);

        RefreshNetworkAccess();
    }

    private void Poll(object? sender, EventArgs args)

    {
        if (_closing) return;

        if (_networkSetup?.HasExited == true)
        {
            var code = _networkSetup.ExitCode;
            _networkSetup.Dispose();
            _networkSetup = null;

            RefreshNetworkAccess();

            if (code != 0) ShowError("Local phone access was not configured. Allow the administrator request and try again.");
        }

        if (_installer?.HasExited == true)

        {
            var exit = _installer.ExitCode;
            _installer.Dispose();
            _installer = null;

            CheckInstallation();

            if (_installed) OutputToggle.IsChecked = true;

            else if (exit != 0) ShowError("The virtual camera was not installed. You can keep using the preview and try installation again.");
        }

        var snapshot = PreviewHost.Snapshot();
        if (snapshot is null)
        {
            PreviewHost.Visibility = Visibility.Hidden;
            return;
        }

        _state = snapshot.Value;

        var live = _state.HasFrame != 0;

        PreviewHost.Visibility = live ? Visibility.Visible : Visibility.Hidden;

        WaitingPanel.Visibility = live ? Visibility.Collapsed : Visibility.Visible;

        var detected = !string.IsNullOrWhiteSpace(_state.Peer) || _state.Packets > 0;

        WaitingTitle.Text = _state.Ready == 0 ? "Starting your studio…" : _state.Running == 0 ? "Reconnecting…" : detected ? "Preparing your camera" : "Connect your phone";

        WaitingDescription.Text = _state.Running != 0

            ? detected ? _state.Packets > 0 ? "Video packets are arriving. Waiting for the first decoded frame." : "Your phone has connected. Waiting for its camera stream."

                : "Open Cortex Sender on the same network.\nYour camera appears here automatically."

            : "Keep Studio open while your phone reconnects.\nReception resumes automatically.";

        RunButton.IsEnabled = _state.Ready != 0;

        var isMediaOnly = _state.Mode == 3;
        var isStandaloneMedia = live && _state.Packets == 0;
        DeviceNameLabel.Text = isMediaOnly || isStandaloneMedia ? "Background Media" : (string.IsNullOrWhiteSpace(_state.DeviceName) ? "Phone camera" : _state.DeviceName);

        SourceState.Text = live ? (isMediaOnly ? "Media playback · Phone camera muted" : (isStandaloneMedia ? "Standalone media · Live" : "Connected · Live")) : _state.Running != 0 ? detected ? "Phone detected · Preparing video" : "Auto-connect ready" : "Reconnecting automatically";

        PeerAddress.Text = string.IsNullOrWhiteSpace(_state.Peer) ? "Wi-Fi · Auto-connect" : _state.Peer;

        LiveLabel.Text = live ? (isMediaOnly ? "MEDIA ONLY" : (isStandaloneMedia ? "MEDIA LIVE" : "LIVE INPUT")) : _state.Running != 0 ? "STANDBY" : "RECONNECTING";

        LiveDot.Fill = live ? (isStandaloneMedia || isMediaOnly ? (Brush)new SolidColorBrush(Color.FromRgb(56, 189, 248)) : Brushes.MediumSpringGreen) : Brushes.Orange;

        SessionStatus.Text = live ? (isMediaOnly ? "MEDIA" : (isStandaloneMedia ? "MEDIA" : "LIVE")) : _state.Running != 0 ? "STANDBY" : "RECONNECTING";
        SessionStatus.Foreground = live ? (isStandaloneMedia || isMediaOnly ? (Brush)new SolidColorBrush(Color.FromRgb(56, 189, 248)) : Brushes.MediumSpringGreen) : Brushes.Orange;

        SessionBadge.Background = new SolidColorBrush(live ? (isStandaloneMedia || isMediaOnly ? Color.FromArgb(40, 2, 132, 199) : Color.FromArgb(40, 16, 185, 129)) : Color.FromArgb(40, 245, 158, 11));
        SessionBadge.BorderBrush = new SolidColorBrush(live ? (isStandaloneMedia || isMediaOnly ? Color.FromArgb(80, 2, 132, 199) : Color.FromArgb(80, 16, 185, 129)) : Color.FromArgb(80, 245, 158, 11));

        PreviewStatus.Text = live ? (!string.IsNullOrWhiteSpace(_state.Status) ? _state.Status : "Preview is live") : _state.Ready == 0 ? "Starting video engine…" : detected ? "Phone handshake confirmed" : "mDNS + LAN discovery";

        FpsStat.Text = live ? (_state.IncomingFps > 0 ? _state.IncomingFps.ToString("0.0") : _state.OutputFps.ToString("0.0")) : "—";

        ResolutionStat.Text = live ? $"{_state.Width} × {_state.Height}" : "—";

        GapsStat.Text = _state.Gaps.ToString("N0");

        PreviewBadge.Text = live ? $"{_state.Width} × {_state.Height}  ·  {(_state.IncomingFps > 0 ? _state.IncomingFps : _state.OutputFps):0} FPS" : $"{_state.OutputFps} FPS OUTPUT";

        DecodeDiagnostic.Text = !live && _state.Packets > 0

            ? _state.Diagnostics.Contains("decodeErrors=0")

                ? "Video has reached Studio. Waiting for a complete keyframe."

                : "The GPU decoder could not open this stream. Connection details are saved in logs/receiver-status.log."

            : "";

        SelectMode(_state.Mode);

        SelectFps(_state.FrameRatePending != 0 ? _state.RequestedPhoneFps : _state.PhoneFps > 0 ? _state.PhoneFps : _state.OutputFps);

        FpsSyncStatus.Text = string.IsNullOrWhiteSpace(_state.Peer)

            ? _state.FrameRatePending != 0 ? $"Will request {_state.RequestedPhoneFps} fps when the phone connects." : "Connect Cortex Sender to synchronize frame rate."

            : _state.PhoneSyncAvailable == 0 ? "Update Cortex Sender to control phone FPS from Windows."

            : _state.FrameRatePending != 0 ? $"Requesting {_state.RequestedPhoneFps} fps on the phone…"

            : _state.PhoneRateFallback != 0

                ? $"Phone uses {_state.PhoneFps} fps. This camera could not enable {_state.RequestedPhoneFps} fps."

                : $"Synced with phone · {_state.PhoneFps} fps. Received FPS may be lower during processing.";

        Fps30Button.IsEnabled = Fps60Button.IsEnabled = _state.PhoneSyncAvailable != 0 || string.IsNullOrWhiteSpace(_state.Peer);

        if (AvatarEnabled.IsChecked == true)
        {
            var separator = _state.Status.IndexOf(" | ", StringComparison.Ordinal);
            AvatarTrackingStatus.Text = AvatarMotion.IsChecked == true
                ? "Demo animation - camera tracking is off"
                : separator >= 0 ? _state.Status[(separator + 3)..] : "Waiting for phone camera - open Cortex Sender";
        }
        if (_avatarMode == 1 && !string.IsNullOrEmpty(_avatarModelPath))
        {
            if (_state.Status.StartsWith("Live2D Avatar", StringComparison.Ordinal))
                CubismSdkNotice.Text = "Model active · textures, motions and physics loaded.";
            else if (!string.IsNullOrWhiteSpace(_state.Error))
                CubismSdkNotice.Text = "Model could not be loaded. See the error above the preview.";
        }
        if (!string.IsNullOrWhiteSpace(_state.Error)) { ShowError(FriendlyError(_state.Error)); _nativeErrorShown = true; }

        else if (_nativeErrorShown) { _nativeErrorShown = false; ErrorBanner.Visibility = Visibility.Collapsed; }

        if (_installed && OutputToggle.IsChecked == true)

            OutputDescription.Text = _state.VirtualSent > 0 ? "Frames are reaching the virtual camera." : "Ready. Select this camera in your webcam app.";

        else if (_installed) OutputDescription.Text = "Camera installed. Output is switched off.";

        var outputOn = _state.VirtualEnabled != 0;

        OutputHeader.Text = outputOn ? _state.VirtualSent > 0 ? "Camera output active" : "Camera output ready" : "Camera output off";

        OutputDot.Fill = outputOn ? Brushes.MediumSpringGreen : Brushes.SlateGray;
        OutputHeader.Foreground = outputOn ? Brushes.MediumSpringGreen : Brushes.LightSlateGray;

        BottomHint.Text = live ? $"Phone stream received · {_state.Frames:N0} frames · {_state.Packets:N0} packets" : $"{_state.Packets:N0} video packets · {_state.Discovery}";
    }

    private static string FriendlyError(string error)

    {
        if (error.Contains("Bind UDP") || error.Contains("bind discovery")) return "The connection port is busy. Close the old console receiver, or change the ports in Connection.";

        if (error.Contains("model", StringComparison.OrdinalIgnoreCase) || error.Contains("ONNX")) return "AI could not load its model. Choose Original, then select a valid RVM FP32 model to try again.";

        return error;
    }

    private void ShowError(string message)
    {
        _nativeErrorShown = false;
        ErrorText.Text = message;
        ErrorBanner.Visibility = Visibility.Visible;
    }

    private void DismissError(object sender, RoutedEventArgs e)
    {
        PreviewHost.Set(11, 0);
        ErrorBanner.Visibility = Visibility.Collapsed;
    }

    private void RefreshNetworkAccess()

    {
        var access = NetworkAccess.Read(_videoPort, _controlPort);

        NetworkAccessLabel.Text = access.Allowed ? $"Local phone access allowed\n{access.Profile}" : $"{access.Profile}\nAllow the phone to reach Studio.";

        AllowPhoneButton.Content = access.Allowed ? "Repair connection" : "Allow phone";

        AllowPhoneButton.IsEnabled = _networkSetup is null;
    }

    private void AllowPhoneClick(object sender, RoutedEventArgs e)

    {
        if (_networkSetup is not null) return;

        var script = Path.Combine(AppContext.BaseDirectory, "scripts", "EnablePhoneConnection.ps1");

        if (!File.Exists(script)) { ShowError("Extract the complete Desktop package. The phone connection helper is missing."); return; }

        try
        {
            var process = new ProcessStartInfo("powershell.exe") { UseShellExecute = true, Verb = "runas", WindowStyle = ProcessWindowStyle.Hidden };

            foreach (var item in new[] { "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script, "-Program", Environment.ProcessPath!, "-VideoPort", _videoPort.ToString(), "-ControlPort", _controlPort.ToString() }) process.ArgumentList.Add(item);

            _networkSetup = Process.Start(process);

            AllowPhoneButton.IsEnabled = false;

            NetworkAccessLabel.Text = "Approve the Windows administrator request to allow your phone on the local network.";
        }

        catch (Win32Exception) { RefreshNetworkAccess(); ShowError("Phone connection setup was cancelled. Click Allow phone connection to try again."); }
    }

    private void SelectMode(int mode)

    {
        if (_displayMode == mode) return;

        _displayMode = mode;

        OriginalButton.Style = (Style)FindResource(mode == 0 ? "Selected" : "ButtonBase");

        ChromaButton.Style = (Style)FindResource(mode == 1 ? "Selected" : "ButtonBase");

        AiButton.Style = (Style)FindResource(mode == 2 ? "Selected" : "ButtonBase");
        if (MediaOnlyButton is not null) MediaOnlyButton.Style = (Style)FindResource(mode == 3 ? "Selected" : "ButtonBase");

        ChromaControls.Visibility = mode == 1 ? Visibility.Visible : Visibility.Collapsed;

        ModeDescription.Text = mode switch { 1 => "Remove green, then refine the edges below.", 2 => "Subject isolation is active. First use may take a moment.", 3 => "Display background image or video directly (no phone camera needed).", _ => "Keep the original camera image." };
    }

    private void SelectFps(int fps)

    {
        if (_displayFps == fps) return;

        _displayFps = fps;

        Fps30Button.Style = (Style)FindResource(fps == 30 ? "Selected" : "ButtonBase");

        Fps60Button.Style = (Style)FindResource(fps == 60 ? "Selected" : "ButtonBase");
    }

    private void RunClick(object sender, RoutedEventArgs e) => PreviewHost.Set(13, 1);

    private void AvatarChanged(object sender, RoutedEventArgs e)
    {
        bool enabled = AvatarEnabled.IsChecked == true;
        if (AvatarDetailsPanel is not null)
            AvatarDetailsPanel.Visibility = enabled ? Visibility.Visible : Visibility.Collapsed;
        if (MotionSourceDescription is not null)
            MotionSourceDescription.Text = AvatarMotion.IsChecked == true
                ? "Animates head, eye and mouth motion in a continuous loop without camera input."
                : "Live head and arm tracking. Move into frame, then set neutral face while looking straight ahead.";
        if (TrackFaceButton is not null)
            TrackFaceButton.Style = (Style)FindResource(AvatarMotion.IsChecked == true ? "ButtonBase" : "Selected");
        if (AvatarRecenterButton is not null)
            AvatarRecenterButton.IsEnabled = enabled && AvatarMotion.IsChecked != true;
        if (AvatarMotionChoice is not null)
        {
            AvatarMotionChoice.IsEnabled = AvatarMotion.IsChecked == true;
            AvatarMotionChoice.Visibility = AvatarMotion.IsChecked == true ? Visibility.Visible : Visibility.Collapsed;
        }
        if (AvatarMotionLabel is not null)
            AvatarMotionLabel.Text = AvatarMotion.IsChecked == true ? "Motion" : "Camera controls the model · idle off";
        PreviewHost.Set(21, _avatarMode);
        PreviewHost.Set(20, AvatarMotion.IsChecked == true ? 1 : 0);
        PreviewHost.Set(19, enabled ? 1 : 0);
    }

    private void TrackFaceClick(object sender, RoutedEventArgs e)
    {
        AvatarEnabled.IsChecked = true;
        AvatarMotion.IsChecked = false;
        AvatarChanged(sender, e);
    }

    private void AvatarRecenterClick(object sender, RoutedEventArgs e)
    {
        PreviewHost.Set(28, 1);
        AvatarTrackingStatus.Text = "Look straight at the camera - centering on the next detected face.";
    }

    private void AvatarSmoothingChanged(object sender, RoutedEventArgs e)
    {
        if (AvatarSmoothing is null || AvatarSmoothingLabel is null || PreviewHost is null) return;
        AvatarSmoothingLabel.Text = $"{AvatarSmoothing.Value:0} ms";
        PreviewHost.Set(27, AvatarSmoothing.Value / 1000);
    }

    private void AvatarTrackingOptionsChanged(object sender, RoutedEventArgs e)
    {
        if (AvatarSensitivity is null || AvatarSensitivityLabel is null || AvatarArms is null || AvatarMirror is null || PreviewHost is null) return;
        AvatarSensitivityLabel.Text = $"{AvatarSensitivity.Value:0.00}x";
        PreviewHost.Set(29, AvatarSensitivity.Value);
        PreviewHost.Set(30, AvatarArms.IsChecked == true ? 1 : 0);
        PreviewHost.Set(31, AvatarMirror.IsChecked == true ? 1 : 0);
    }

    private void AvatarTestClick(object sender, RoutedEventArgs e)
    {
        _avatarMode = 0;
        AvatarTestBtn.Style = (Style)FindResource("Selected");
        AvatarCubismBtn.Style = (Style)FindResource("ButtonBase");
        CubismModelCard.Visibility = Visibility.Collapsed;
        PreviewHost.Set(21, 0);
    }

    private void AvatarCubismClick(object sender, RoutedEventArgs e)
    {
        _avatarMode = 1;
        AvatarCubismBtn.Style = (Style)FindResource("Selected");
        AvatarTestBtn.Style = (Style)FindResource("ButtonBase");
        CubismModelCard.Visibility = Visibility.Visible;
        PreviewHost.Set(21, 1);
    }

    private void ChooseCubismModelClick(object sender, RoutedEventArgs e)
    {
        var dialog = new Microsoft.Win32.OpenFileDialog
        {
            Title = "Choose Live2D Cubism model",
            Filter = "Live2D model settings|*.model3.json|Live2D model data|*.moc3",
            CheckFileExists = true
        };
        if (dialog.ShowDialog(this) == true)
            LoadAvatar(dialog.FileName);
    }

    private void LoadHaruClick(object sender, RoutedEventArgs e)
    {
        var model = Path.Combine(AppContext.BaseDirectory, "assets", "avatars", "Haru", "Haru.model3.json");
        if (!File.Exists(model))
        {
            CubismSdkNotice.Text = "The Haru sample is missing. Choose a model folder manually.";
            return;
        }
        LoadAvatar(model);
        AvatarEnabled.IsChecked = true;
        AvatarMotion.IsChecked = false;
        AvatarChanged(sender, e);
    }

    private void LoadAvatar(string filename)
    {
        try
        {
            if (Path.GetExtension(filename).Equals(".moc3", StringComparison.OrdinalIgnoreCase))
                filename = Path.ChangeExtension(filename, ".model3.json");
            if (new FileInfo(filename).Length > 4 * 1024 * 1024)
                throw new InvalidDataException("Model settings file is too large.");
            using var document = JsonDocument.Parse(File.ReadAllText(filename));
            var references = document.RootElement.GetProperty("FileReferences");
            var motions = new List<string> { "Automatic idle" };
            var expressions = new List<string> { "Default expression" };
            if (references.TryGetProperty("Motions", out var groups))
                foreach (var group in groups.EnumerateObject())
                    foreach (var item in group.Value.EnumerateArray())
                        motions.Add(group.Name + " / " + Path.GetFileNameWithoutExtension(item.GetProperty("File").GetString()));
            if (references.TryGetProperty("Expressions", out var entries))
                foreach (var item in entries.EnumerateArray())
                    expressions.Add(item.GetProperty("Name").GetString() ?? "Expression");
            _loadingAvatarChoices = true;
            try
            {
                AvatarMotionChoice.ItemsSource = motions;
                AvatarExpressionChoice.ItemsSource = expressions;
                AvatarMotionChoice.SelectedIndex = AvatarExpressionChoice.SelectedIndex = 0;
            }
            finally { _loadingAvatarChoices = false; }
            _avatarModelPath = filename;
            CubismModelName.Text = Path.GetFileName(filename);
            CubismModelName.ToolTip = filename;
            CubismSdkNotice.Text = "Loading model. Any asset or renderer error appears above the preview.";
            PreviewHost.Set(22, -1);
            PreviewHost.Set(23, -1);
            PreviewHost.SetPath(4, filename);
            AvatarCubismClick(this, new RoutedEventArgs());
        }
        catch (Exception error) when (error is IOException or JsonException or KeyNotFoundException or InvalidOperationException or UnauthorizedAccessException)
        {
            CubismSdkNotice.Text = "Could not read model settings. " + error.Message;
        }
    }

    private void AvatarFramingChanged(object sender, RoutedEventArgs e)
    {
        if (AvatarScale is null || AvatarPositionX is null || AvatarPositionY is null ||
            AvatarScaleLabel is null || AvatarXLabel is null || AvatarYLabel is null ||
            PreviewHost is null) return;
        AvatarScaleLabel.Text = $"{AvatarScale.Value:0.00}x";
        AvatarXLabel.Text = $"{AvatarPositionX.Value * 100:0}%";
        AvatarYLabel.Text = $"{AvatarPositionY.Value * 100:0}%";
        PreviewHost.Set(24, AvatarScale.Value);
        PreviewHost.Set(25, AvatarPositionX.Value);
        PreviewHost.Set(26, AvatarPositionY.Value);
    }

    private void SetAvatarFraming(double scale, double x, double y)
    {
        AvatarScale.Value = scale;
        AvatarPositionX.Value = x;
        AvatarPositionY.Value = y;
    }

    private void AvatarUpperBodyClick(object sender, RoutedEventArgs e) => SetAvatarFraming(1.8, 0, 0);
    private void AvatarFaceClick(object sender, RoutedEventArgs e) => SetAvatarFraming(3.2, 0, -0.05);
    private void AvatarResetFramingClick(object sender, RoutedEventArgs e) => SetAvatarFraming(1, 0, 0);

    private void AvatarAnimationChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loadingAvatarChoices || !_initialized) return;
        PreviewHost.Set(22, AvatarMotionChoice.SelectedIndex - 1);
        PreviewHost.Set(23, AvatarExpressionChoice.SelectedIndex - 1);
    }

    private void OriginalClick(object sender, RoutedEventArgs e)
    {
        PreviewHost.Set(2, 0);
        SelectMode(0);
    }

    private void ChromaClick(object sender, RoutedEventArgs e)
    {
        PreviewHost.Set(2, 1);
        SelectMode(1);
    }

    private void AiClick(object sender, RoutedEventArgs e)

    {
        if (!File.Exists(_model))

        {
            var dialog = new OpenFileDialog { Title = "Select the RVM MobileNetV3 FP32 model", Filter = "ONNX model|*.onnx", CheckFileExists = true };

            if (dialog.ShowDialog(this) != true) return;

            _model = dialog.FileName;
            PreviewHost.SetPath(3, _model);
        }

        PreviewHost.Set(2, 2);
        SelectMode(2);
    }

    private void MediaOnlyClick(object sender, RoutedEventArgs e)
    {
        PreviewHost.Set(2, 3);
        SelectMode(3);
    }
    private void SimilarityChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_initialized)
        {
            PreviewHost.Set(5, e.NewValue / 100);
        }
    }

    private void SoftnessChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_initialized)
        {
            PreviewHost.Set(6, e.NewValue / 100);
        }
    }

    private void SpillChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_initialized)
        {
            PreviewHost.Set(7, e.NewValue / 100);
        }
    }

    private void TransparencyChanged(object sender, RoutedEventArgs e)
    {
        if (_initialized)
        {
            PreviewHost.Set(8, TransparentToggle.IsChecked == true ? 1 : 0);
        }
    }

    private void Fps30Click(object sender, RoutedEventArgs e)
    {
        PreviewHost.Set(14, 30);
        SelectFps(30);
    }

    private void Fps60Click(object sender, RoutedEventArgs e)
    {
        PreviewHost.Set(14, 60);
        SelectFps(60);
    }

    private void ImageClick(object sender, RoutedEventArgs e) => ChooseBackground(false);

    private void VideoClick(object sender, RoutedEventArgs e) => ChooseBackground(true);

    private void ChooseBackground(bool video)

    {
        var dialog = new OpenFileDialog { Title = video ? "Choose an H.264 background video" : "Choose a background image", Filter = video ? "H.264 video|*.mp4;*.mkv;*.mov|All files|*.*" : "Images|*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff|All files|*.*", CheckFileExists = true };

        if (dialog.ShowDialog(this) != true) return;

        var prepared = dialog.FileName;

        if (prepared is null) return;

        PreviewHost.SetPath(video ? 2 : 1, prepared);
        UpdateBackgroundCard(prepared, video);

        if (_state.Mode == 0 || (_state.Packets == 0 && _state.Mode != 3)) { var targetMode = _state.Packets == 0 ? 3 : 2; PreviewHost.Set(2, targetMode); SelectMode(targetMode); }
    }

    private void SourceDetailsClick(object sender, MouseButtonEventArgs e)

    {
        var panel = new StackPanel();

        panel.Children.Add(new TextBlock { Text = DeviceNameLabel.Text, FontSize = 22, FontWeight = FontWeights.SemiBold });

        panel.Children.Add(new TextBlock { Text = $"{SourceState.Text}\n{PeerAddress.Text}\n\nCamera output: {_state.Width} × {_state.Height}\nReceived frame rate: {_state.IncomingFps:0.0} fps\nFrames received: {_state.Frames:N0}", TextWrapping = TextWrapping.Wrap, LineHeight = 25, Margin = new Thickness(0, 18, 0, 0) });

        Dialog("Phone details", panel, 330).ShowDialog();
    }

    private readonly List<string> _savedBackgrounds = new();
    private string _currentBackgroundPath = "";

    private static string BackgroundsFilePath()
    {
        var dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "CortexStudio");
        Directory.CreateDirectory(dir);
        return Path.Combine(dir, "backgrounds.txt");
    }

    private void LoadSavedBackgrounds()
    {
        try
        {
            var file = BackgroundsFilePath();
            if (File.Exists(file))
            {
                foreach (var line in File.ReadAllLines(file))
                {
                    var trimmed = line.Trim();
                    if (!string.IsNullOrEmpty(trimmed) && File.Exists(trimmed) && !_savedBackgrounds.Contains(trimmed))
                        _savedBackgrounds.Add(trimmed);
                }
            }
        }
        catch { }

        var defaultBmp = Path.Combine(AppContext.BaseDirectory, "assets", "background.bmp");
        if (File.Exists(defaultBmp) && !_savedBackgrounds.Contains(defaultBmp))
            _savedBackgrounds.Add(defaultBmp);

        if (!string.IsNullOrEmpty(_initialBackground) && File.Exists(_initialBackground) && !_savedBackgrounds.Contains(_initialBackground))
            _savedBackgrounds.Insert(0, _initialBackground);

        RenderBackgroundGallery();
    }

    private void SaveBackgrounds()
    {
        try
        {
            File.WriteAllLines(BackgroundsFilePath(), _savedBackgrounds);
        }
        catch { }
    }

    private void AddSavedBackground(string path)
    {
        if (string.IsNullOrWhiteSpace(path) || !File.Exists(path)) return;
        _savedBackgrounds.Remove(path);
        _savedBackgrounds.Insert(0, path);
        if (_savedBackgrounds.Count > 24) _savedBackgrounds.RemoveAt(_savedBackgrounds.Count - 1);
        SaveBackgrounds();
        RenderBackgroundGallery();
    }

    private void RemoveSavedBackground(string path)
    {
        _savedBackgrounds.Remove(path);
        SaveBackgrounds();
        if (_currentBackgroundPath == path)
        {
            ClearBackgroundClick(this, new RoutedEventArgs());
        }
        else
        {
            RenderBackgroundGallery();
        }
    }

    private readonly Dictionary<string, ImageSource> _thumbnailCache = new(StringComparer.OrdinalIgnoreCase);
    private readonly HashSet<string> _pendingThumbnails = new(StringComparer.OrdinalIgnoreCase);

    private static string GetVideoThumbDiskPath(string videoPath)
    {
        try
        {
            var dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "CortexStudio", "thumbs");
            Directory.CreateDirectory(dir);
            var hash = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(videoPath.ToLowerInvariant())));
            return Path.Combine(dir, $"{hash}.jpg");
        }
        catch { return ""; }
    }

    private ImageSource? GetVideoThumbnail(string videoPath, Action<ImageSource>? onLoaded = null)
    {
        if (string.IsNullOrWhiteSpace(videoPath) || !File.Exists(videoPath)) return null;

        if (_thumbnailCache.TryGetValue(videoPath, out var cached))
            return cached;

        var diskPath = GetVideoThumbDiskPath(videoPath);
        if (!string.IsNullOrEmpty(diskPath) && File.Exists(diskPath))
        {
            try
            {
                var bmp = new BitmapImage();
                bmp.BeginInit();
                bmp.CacheOption = BitmapCacheOption.OnLoad;
                bmp.UriSource = new Uri(diskPath, UriKind.Absolute);
                bmp.EndInit();
                bmp.Freeze();
                _thumbnailCache[videoPath] = bmp;
                return bmp;
            }
            catch { }
        }

        if (_pendingThumbnails.Contains(videoPath)) return null;
        _pendingThumbnails.Add(videoPath);

        ExtractVideoThumbnailAsync(videoPath, diskPath, onLoaded);
        return null;
    }

    private void ExtractVideoThumbnailAsync(string videoPath, string diskPath, Action<ImageSource>? onLoaded)
    {
        Dispatcher.InvokeAsync(async () =>
        {
            try
            {
                var player = new MediaPlayer();
                player.ScrubbingEnabled = true;
                var tcs = new TaskCompletionSource<BitmapSource?>();

                void CleanUp()
                {
                    try { player.Close(); } catch { }
                }

                player.MediaOpened += (s, e) =>
                {
                    try
                    {
                        var seek = TimeSpan.FromSeconds(Math.Min(0.5, player.NaturalDuration.HasTimeSpan && player.NaturalDuration.TimeSpan.TotalSeconds > 1 ? 0.5 : 0.0));
                        player.Position = seek;
                        var timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(200) };
                        timer.Tick += (ts, te) =>
                        {
                            timer.Stop();
                            try
                            {
                                var visual = new DrawingVisual();
                                using (var dc = visual.RenderOpen())
                                {
                                    dc.DrawVideo(player, new Rect(0, 0, 240, 135));
                                }
                                var target = new RenderTargetBitmap(240, 135, 96, 96, PixelFormats.Pbgra32);
                                target.Render(visual);

                                if (!string.IsNullOrEmpty(diskPath))
                                {
                                    var encoder = new JpegBitmapEncoder { QualityLevel = 85 };
                                    encoder.Frames.Add(BitmapFrame.Create(target));
                                    using var fs = File.Create(diskPath);
                                    encoder.Save(fs);
                                }

                                tcs.TrySetResult(target);
                            }
                            catch (Exception ex) { tcs.TrySetException(ex); }
                            finally { CleanUp(); }
                        };
                        timer.Start();
                    }
                    catch (Exception ex) { tcs.TrySetException(ex); CleanUp(); }
                };

                player.MediaFailed += (s, e) =>
                {
                    CleanUp();
                    tcs.TrySetResult(null);
                };

                player.Open(new Uri(videoPath, UriKind.Absolute));
                player.Pause();

                var completed = await Task.WhenAny(tcs.Task, Task.Delay(4000));
                BitmapSource? result = null;
                if (completed == tcs.Task)
                    result = await tcs.Task;
                else
                    CleanUp();

                _pendingThumbnails.Remove(videoPath);
                if (result is not null)
                {
                    _thumbnailCache[videoPath] = result;
                    onLoaded?.Invoke(result);
                    RenderBackgroundGallery();
                    if (_currentBackgroundPath == videoPath)
                    {
                        BackgroundThumbnail.Source = result;
                    }
                }
            }
            catch
            {
                _pendingThumbnails.Remove(videoPath);
            }
        });
    }

    private void RenderBackgroundGallery()
    {
        if (BackgroundGallery is null) return;
        BackgroundGallery.Children.Clear();
        if (GalleryCountLabel is not null)
            GalleryCountLabel.Text = _savedBackgrounds.Count.ToString();

        var noneBorder = new Border
        {
            Width = 74,
            Height = 48,
            CornerRadius = new CornerRadius(8),
            Background = new SolidColorBrush(Color.FromRgb(16, 23, 38)),
            BorderBrush = new SolidColorBrush(string.IsNullOrEmpty(_currentBackgroundPath) ? Color.FromRgb(0, 210, 255) : Color.FromRgb(31, 46, 69)),
            BorderThickness = new Thickness(string.IsNullOrEmpty(_currentBackgroundPath) ? 2 : 1),
            Margin = new Thickness(0, 0, 7, 7),
            Cursor = Cursors.Hand,
            ToolTip = "Remove background (Solid / None)"
        };
        var nonePanel = new StackPanel { HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
        var banPath = new System.Windows.Shapes.Path
        {
            Data = (Geometry)FindResource("BanIcon"),
            Stroke = new SolidColorBrush(string.IsNullOrEmpty(_currentBackgroundPath) ? Color.FromRgb(0, 210, 255) : Color.FromRgb(100, 116, 139)),
            StrokeThickness = 1.4,
            Width = 14,
            Height = 14,
            HorizontalAlignment = HorizontalAlignment.Center
        };
        var noneText = new TextBlock
        {
            Text = "None",
            FontSize = 10,
            FontWeight = FontWeights.SemiBold,
            Foreground = new SolidColorBrush(string.IsNullOrEmpty(_currentBackgroundPath) ? Color.FromRgb(0, 210, 255) : Color.FromRgb(148, 163, 184)),
            HorizontalAlignment = HorizontalAlignment.Center,
            Margin = new Thickness(0, 3, 0, 0)
        };
        nonePanel.Children.Add(banPath);
        nonePanel.Children.Add(noneText);
        noneBorder.Child = nonePanel;
        noneBorder.MouseLeftButtonUp += (s, e) => ClearBackgroundClick(s, e);
        BackgroundGallery.Children.Add(noneBorder);

        foreach (var path in _savedBackgrounds.ToArray())
        {
            if (!File.Exists(path)) continue;
            var ext = Path.GetExtension(path).ToLowerInvariant();
            bool isVideo = ext is ".mp4" or ".mkv" or ".mov";
            bool isCurrent = path == _currentBackgroundPath;

            var tile = new Border
            {
                Width = 74,
                Height = 48,
                CornerRadius = new CornerRadius(8),
                Background = new SolidColorBrush(Color.FromRgb(12, 18, 30)),
                BorderBrush = new SolidColorBrush(isCurrent ? Color.FromRgb(0, 210, 255) : Color.FromRgb(31, 46, 69)),
                BorderThickness = new Thickness(isCurrent ? 2 : 1),
                Margin = new Thickness(0, 0, 7, 7),
                Cursor = Cursors.Hand,
                ClipToBounds = true,
                ToolTip = Path.GetFileName(path) + " (Click to switch · Right-click to remove)"
            };

            var grid = new Grid();

            if (!isVideo)
            {
                try
                {
                    var bmp = new BitmapImage();
                    bmp.BeginInit();
                    bmp.CacheOption = BitmapCacheOption.OnLoad;
                    bmp.DecodePixelWidth = 150;
                    bmp.UriSource = new Uri(path, UriKind.Absolute);
                    bmp.EndInit();
                    bmp.Freeze();
                    grid.Children.Add(new Image { Source = bmp, Stretch = Stretch.UniformToFill });
                }
                catch { }
            }
            else
            {
                var thumb = GetVideoThumbnail(path);
                if (thumb is not null)
                {
                    grid.Children.Add(new Image { Source = thumb, Stretch = Stretch.UniformToFill });
                }
                else
                {
                    grid.Children.Add(new Border { Background = new SolidColorBrush(Color.FromRgb(10, 20, 35)) });
                }

                var videoBadge = new Border
                {
                    Background = new SolidColorBrush(Color.FromArgb(190, 0, 0, 0)),
                    CornerRadius = new CornerRadius(3),
                    Padding = new Thickness(4, 1, 4, 1),
                    HorizontalAlignment = HorizontalAlignment.Left,
                    VerticalAlignment = VerticalAlignment.Bottom,
                    Margin = new Thickness(3)
                };
                videoBadge.Child = new TextBlock { Text = "VIDEO", FontSize = 7, FontWeight = FontWeights.Bold, Foreground = new SolidColorBrush(Color.FromRgb(56, 189, 248)) };
                grid.Children.Add(videoBadge);
            }

            var menu = new ContextMenu();
            var delItem = new MenuItem
            {
                Header = "Remove from gallery",
                Foreground = new SolidColorBrush(Color.FromRgb(248, 113, 113))
            };
            delItem.Click += (s, e) => RemoveSavedBackground(path);
            menu.Items.Add(delItem);
            tile.ContextMenu = menu;

            tile.Child = grid;
            tile.MouseLeftButtonUp += (s, e) =>
            {
                PreviewHost.SetPath(isVideo ? 2 : 1, path);
                UpdateBackgroundCard(path, isVideo);
                if (_state.Mode == 0 || (_state.Packets == 0 && _state.Mode != 3))
                {
                    var targetMode = _state.Packets == 0 ? 3 : 2; PreviewHost.Set(2, targetMode); SelectMode(targetMode);
                }
            };

            BackgroundGallery.Children.Add(tile);
        }
    }

    private void BackgroundTransformChanged(object sender, RoutedEventArgs e)
    {
        if (PreviewHost is null || BackgroundZoom is null || BackgroundX is null || BackgroundY is null || BackgroundFit is null) return;
        PreviewHost.Set(15, BackgroundZoom.Value);
        PreviewHost.Set(16, BackgroundX.Value);
        PreviewHost.Set(17, BackgroundY.Value);
        PreviewHost.Set(18, BackgroundFit.IsChecked == true ? 1 : 0);
        if (ZoomValueLabel is not null) ZoomValueLabel.Text = $"{BackgroundZoom.Value:0.0}x";
        if (PosXValueLabel is not null) PosXValueLabel.Text = $"{(int)(BackgroundX.Value * 100)}%";
        if (PosYValueLabel is not null) PosYValueLabel.Text = $"{(int)(BackgroundY.Value * 100)}%";
    }
    private void ResetFramingClick(object sender, RoutedEventArgs e)
    {
        BackgroundZoom.Value = 1;
        BackgroundX.Value = BackgroundY.Value = .5;
        BackgroundFit.IsChecked = false;
        BackgroundTransformChanged(sender, e);
    }
    private void ClearBackgroundClick(object sender, RoutedEventArgs e)

    {
        PreviewHost.SetPath(1, ""); UpdateBackgroundCard("", false); if (_state.Mode == 3) { PreviewHost.Set(2, 0); SelectMode(0); }
    }

    private void UpdateBackgroundCard(string path, bool video)
    {
        _currentBackgroundPath = path;
        BackgroundName.Text = path.Length > 0 ? Path.GetFileName(path) : "Solid studio background";
        BackgroundName.ToolTip = path.Length > 0 ? path : null;
        BackgroundThumbnail.Source = null;
        BackgroundPlaceholder.Visibility = path.Length == 0 ? Visibility.Visible : Visibility.Collapsed;
        if (VideoPlaceholder is not null)
            VideoPlaceholder.Visibility = video && path.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        if (BackgroundStatusLabel is not null)
            BackgroundStatusLabel.Text = path.Length > 0 ? (video ? "Video active" : "Image active") : "Default / None";
        if (RemoveBackgroundBtn is not null)
        {
            RemoveBackgroundBtn.IsEnabled = path.Length > 0;
            RemoveBackgroundBtn.Opacity = path.Length > 0 ? 1.0 : 0.4;
        }
        if (path.Length > 0)
        {
            if (!video)
            {
                try
                {
                    var image = new BitmapImage();
                    image.BeginInit();
                    image.CacheOption = BitmapCacheOption.OnLoad;
                    image.DecodePixelWidth = 320;
                    image.UriSource = new Uri(path, UriKind.Absolute);
                    image.EndInit();
                    image.Freeze();
                    BackgroundThumbnail.Source = image;
                }
                catch { }
            }
            else
            {
                var thumb = GetVideoThumbnail(path, loaded =>
                {
                    if (_currentBackgroundPath == path)
                    {
                        BackgroundThumbnail.Source = loaded;
                    }
                });
                if (thumb is not null)
                {
                    BackgroundThumbnail.Source = thumb;
                }
            }
            AddSavedBackground(path);
        }
        else
        {
            RenderBackgroundGallery();
        }
    }
    private void OutputChanged(object sender, RoutedEventArgs e)

    {
        if (!_initialized) return;

        if (OutputToggle.IsChecked == true && !_installed)

        {
            OutputToggle.IsChecked = false;

            OutputDescription.Text = "Install the virtual camera once, then enable output.";

            return;
        }

        PreviewHost.Set(3, OutputToggle.IsChecked == true ? 1 : 0);
    }

    private static bool CameraInstalled()

    {
        try

        {
            using var category = Registry.ClassesRoot.OpenSubKey(@"CLSID\{860BB310-5D01-11D0-BD3B-00A0C911CE86}\Instance");

            if (category is null) return false;

            foreach (var key in category.GetSubKeyNames())
            {
                using var device = category.OpenSubKey(key);

                if ((device?.GetValue("FriendlyName") as string)?.Contains("Unity Video Capture", StringComparison.OrdinalIgnoreCase) != true) continue;

                var clsid = device?.GetValue("CLSID") as string;

                if (string.IsNullOrWhiteSpace(clsid)) continue;

                using var server = Registry.ClassesRoot.OpenSubKey($@"CLSID\{clsid}\InprocServer32");

                var binary = Environment.ExpandEnvironmentVariables((server?.GetValue(null) as string ?? "").Trim('"'));

                if (File.Exists(binary)) return true;
            }
        }

        catch { }

        return false;
    }

    private void CheckInstallation()

    {
        _installed = CameraInstalled();

        InstallButton.Content = _installed ? "Virtual camera installed" : "Set up virtual camera";

        InstallButton.IsEnabled = !_installed;

        OutputDescription.Text = _installed ? "Ready for your webcam apps." : "Camera setup is missing or needs repair. Preview still works.";
    }

    private void InstallClick(object sender, RoutedEventArgs e)

    {
        var script = Path.Combine(AppContext.BaseDirectory, "scripts", "InstallVirtualCamera.ps1");

        if (!File.Exists(script)) { ShowError("The camera installer is missing. Extract the full Desktop package before running Studio."); return; }

        try

        {
            _installer = Process.Start(new ProcessStartInfo("powershell.exe", $"-NoProfile -ExecutionPolicy Bypass -File \"{script}\"") { UseShellExecute = true, Verb = "runas", WindowStyle = ProcessWindowStyle.Hidden });

            InstallButton.IsEnabled = false; InstallButton.Content = "Installing…";
        }

        catch (Win32Exception) { CheckInstallation(); OutputDescription.Text = "Installation was cancelled. Your preview still works."; }
    }

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int attrValue, int attrSize);
    private const int DWMWA_USE_IMMERSIVE_DARK_MODE = 20;

    private Window Dialog(string title, FrameworkElement body, double height)
    {
        var window = new Window
        {
            Title = title,
            Owner = this,
            Width = 490,
            Height = height + 40,
            WindowStartupLocation = WindowStartupLocation.CenterOwner,
            ResizeMode = ResizeMode.NoResize,
            WindowStyle = WindowStyle.None,
            Background = (Brush)FindResource("Page"),
            Foreground = Brushes.White,
            FontFamily = new FontFamily("Segoe UI")
        };

        var chrome = new WindowChrome
        {
            CaptionHeight = 40,
            ResizeBorderThickness = new Thickness(0),
            GlassFrameThickness = new Thickness(0),
            CornerRadius = new CornerRadius(0),
            UseAeroCaptionButtons = false
        };
        WindowChrome.SetWindowChrome(window, chrome);

        var rootBorder = new Border
        {
            BorderBrush = new SolidColorBrush(Color.FromRgb(24, 35, 54)),
            BorderThickness = new Thickness(1),
            Background = (Brush)FindResource("Page")
        };

        var rootGrid = new Grid();
        rootGrid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(40) });
        rootGrid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

        var titleBar = new Grid
        {
            Background = new SolidColorBrush(Color.FromRgb(9, 13, 21))
        };
        titleBar.MouseLeftButtonDown += (s, e) =>
        {
            if (e.LeftButton == MouseButtonState.Pressed) window.DragMove();
        };

        titleBar.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        titleBar.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        var leftStack = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(16, 0, 0, 0)
        };

        var iconBadge = new Border
        {
            Width = 20,
            Height = 20,
            CornerRadius = new CornerRadius(5),
            Background = new SolidColorBrush(Color.FromRgb(12, 37, 61)),
            BorderBrush = new SolidColorBrush(Color.FromRgb(2, 132, 199)),
            BorderThickness = new Thickness(1),
            Margin = new Thickness(0, 0, 10, 0)
        };
        var viewbox = new Viewbox { Width = 11, Height = 11 };
        var logoPath = new System.Windows.Shapes.Path
        {
            Fill = new SolidColorBrush(Color.FromRgb(0, 210, 255)),
            Data = Geometry.Parse("M4,0 L20,0 L24,4 L24,9 L18,9 L18,6 L7,6 L6,7 L6,17 L7,18 L18,18 L18,15 L24,15 L24,20 L20,24 L4,24 L0,20 L0,4 Z")
        };
        viewbox.Child = logoPath;
        iconBadge.Child = viewbox;
        leftStack.Children.Add(iconBadge);

        var titleText = new TextBlock
        {
            Text = title,
            FontSize = 12,
            FontWeight = FontWeights.Bold,
            Foreground = new SolidColorBrush(Color.FromRgb(248, 250, 252)),
            VerticalAlignment = VerticalAlignment.Center
        };
        leftStack.Children.Add(titleText);
        Grid.SetColumn(leftStack, 0);
        titleBar.Children.Add(leftStack);

        var rightStack = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            VerticalAlignment = VerticalAlignment.Center
        };
        WindowChrome.SetIsHitTestVisibleInChrome(rightStack, true);

        var closeBtn = new Button
        {
            Style = (Style)FindResource("WindowButton"),
            ToolTip = "Close",
            Width = 44,
            Height = 40
        };
        var closeIcon = new System.Windows.Shapes.Path
        {
            Data = (Geometry)FindResource("CloseIcon"),
            Style = (Style)FindResource("Icon"),
            Width = 11,
            Height = 11
        };
        closeBtn.Content = closeIcon;
        closeBtn.Click += (_, _) => window.Close();
        rightStack.Children.Add(closeBtn);

        Grid.SetColumn(rightStack, 1);
        titleBar.Children.Add(rightStack);

        Grid.SetRow(titleBar, 0);
        rootGrid.Children.Add(titleBar);

        var bodyBorder = new Border
        {
            Padding = new Thickness(26),
            Child = body
        };
        Grid.SetRow(bodyBorder, 1);
        rootGrid.Children.Add(bodyBorder);

        rootBorder.Child = rootGrid;
        window.Content = rootBorder;

        window.SourceInitialized += (s, e) =>
        {
            try
            {
                var handle = new System.Windows.Interop.WindowInteropHelper(window).Handle;
                if (handle != IntPtr.Zero)
                {
                    int darkMode = 1;
                    DwmSetWindowAttribute(handle, DWMWA_USE_IMMERSIVE_DARK_MODE, ref darkMode, sizeof(int));
                }
            }
            catch { }
        };

        return window;
    }

    private void ConnectionClick(object sender, RoutedEventArgs e)

    {
        var panel = new StackPanel();

        panel.Children.Add(new TextBlock { Text = "Connect your phone", FontSize = 23, FontWeight = FontWeights.SemiBold });

        panel.Children.Add(new TextBlock { Text = "Open Cortex Sender on the same local network. Auto-connect is enabled while the studio is running.", TextWrapping = TextWrapping.Wrap, Foreground = (Brush)FindResource("Muted"), Margin = new Thickness(0, 12, 0, 20) });

        panel.Children.Add(new TextBlock { Text = "THIS PC", FontSize = 10, Foreground = (Brush)FindResource("Muted") });

        var address = string.Join("  ·  ", _addresses);

        panel.Children.Add(new TextBlock { Text = address.Length > 0 ? address : "No local IPv4 address found", Margin = new Thickness(0, 8, 0, 20), TextWrapping = TextWrapping.Wrap, FontSize = 16 });

        var ports = new Grid();
        ports.ColumnDefinitions.Add(new ColumnDefinition());
        ports.ColumnDefinitions.Add(new ColumnDefinition());

        var videoBox = new TextBox { Text = _videoPort.ToString(), Margin = new Thickness(0, 0, 8, 0) };

        var controlBox = new TextBox { Text = _controlPort.ToString(), Margin = new Thickness(8, 0, 0, 0) };

        var labels = new Grid();
        labels.ColumnDefinitions.Add(new ColumnDefinition());
        labels.ColumnDefinitions.Add(new ColumnDefinition());
        labels.Children.Add(new TextBlock { Text = "Video UDP port", FontSize = 11 });

        var label = new TextBlock
        {
            Text = "Connection UDP port",
            FontSize = 11,
            Margin = new Thickness(8, 0, 0, 0)
        };
        Grid.SetColumn(label, 1);
        labels.Children.Add(label);
        panel.Children.Add(labels);

        Grid.SetColumn(controlBox, 1);
        ports.Children.Add(videoBox);
        ports.Children.Add(controlBox);
        ports.Margin = new Thickness(0, 8, 0, 14);
        panel.Children.Add(ports);

        panel.Children.Add(new TextBlock { Text = "The phone uses mDNS and LAN discovery automatically. If it cannot connect, use Allow phone connection in the sidebar. For manual setup, enter this PC address and the CONNECTION port in the updated Android app. Both devices must use the same router; guest Wi-Fi may isolate them.", TextWrapping = TextWrapping.Wrap, Foreground = (Brush)FindResource("Muted"), FontSize = 11, LineHeight = 19 });

        var apply = new Button
        {
            Content = "Apply connection settings",
            Style = (Style)FindResource("Primary"),
            Margin = new Thickness(0, 22, 0, 0)
        };
        panel.Children.Add(apply);

        var dialog = Dialog("Connection · Cortex Studio", panel, 495);

        apply.Click += (_, _) =>
        {
            if (!int.TryParse(videoBox.Text, out var vp) ||
                !int.TryParse(controlBox.Text, out var cp) ||
                vp is < 1 or > 65535 ||
                cp is < 1 or > 65535 ||
                vp == cp)
            {
                MessageBox.Show(dialog, "Choose two different ports between 1 and 65535.", "Connection");
                return;
            }
            _videoPort = vp;
            _controlPort = cp;
            PreviewHost.Set(12, vp + cp * 65536.0);
            dialog.Close();
        };

        dialog.ShowDialog();
    }

    private void HelpClick(object sender, RoutedEventArgs e)

    {
        var panel = new StackPanel();
        panel.Children.Add(new TextBlock
        {
            Text = "Go live in three steps.",
            FontSize = 24,
            FontWeight = FontWeights.SemiBold
        });

        var steps = new[] { "01   Open the Android app\nInstall Cortex Sender and allow camera access.", "02   Join the same network\nYour phone finds this studio automatically. For a wired PC, use the same router.", "03   Preview, then output\nYour video appears right here. Install the virtual camera once to use Unity Video Capture in other apps." };

        foreach (var step in steps) panel.Children.Add(new TextBlock { Text = step, TextWrapping = TextWrapping.Wrap, LineHeight = 23, Margin = new Thickness(0, 22, 0, 0), Foreground = (Brush)FindResource("Muted") });

        panel.Children.Add(new TextBlock { Text = "Start with Original mode at 30 fps to judge camera quality. For 60 fps, also enable 60 fps in the phone app before reconnecting. AI may run slower, depending on your GPU.", TextWrapping = TextWrapping.Wrap, FontSize = 11, Foreground = (Brush)FindResource("Muted"), Margin = new Thickness(0, 24, 0, 0), LineHeight = 19 });

        Dialog("Quick start · Cortex Studio", panel, 530).ShowDialog();
    }

    private void StudioClick(object sender, RoutedEventArgs e) => PreviewHost.Focus();

    private void FocusClick(object sender, RoutedEventArgs e) => SetPreviewFocus(!_focusPreview);

    private void InspectorSelectionChanged(object sender, SelectionChangedEventArgs e)

    {
        if (!_initialized || e.Source != InspectorTabs || !SystemParameters.ClientAreaAnimation) return;

        Dispatcher.InvokeAsync(() =>
        {
            if (InspectorTabs.SelectedContent is UIElement content)

                content.BeginAnimation(OpacityProperty, new DoubleAnimation(0.65, 1, TimeSpan.FromMilliseconds(160)) { FillBehavior = FillBehavior.Stop });
        });
    }

    private void SetPreviewFocus(bool focus)
    {
        if (_focusPreview == focus) return;
        if (focus)
        {
            _sourceWidth = SourcesColumn.Width;
            _inspectorWidth = InspectorColumn.Width;
        }
        _focusPreview = focus;
        SourcesColumn.MinWidth = focus ? 0 : 180;
        InspectorColumn.MinWidth = focus ? 0 : 295;
        SourcesColumn.Width = focus ? new GridLength(0) : _sourceWidth;
        InspectorColumn.Width = focus ? new GridLength(0) : _inspectorWidth;
        SourcesPanel.Visibility = InspectorPanel.Visibility = focus ? Visibility.Collapsed : Visibility.Visible;
        if (SourcesSplitter is not null) SourcesSplitter.Visibility = focus ? Visibility.Collapsed : Visibility.Visible;
        if (InspectorSplitter is not null) InspectorSplitter.Visibility = focus ? Visibility.Collapsed : Visibility.Visible;
        FocusButton.ToolTip = focus ? "Exit focus preview · F11 or Esc" : "Focus preview · F11";
        FocusButton.Background = focus ? (Brush)FindResource("Line") : Brushes.Transparent;
    }
    private void WindowKeyDown(object sender, KeyEventArgs e)

    {
        if (e.Key == Key.F11) { SetPreviewFocus(!_focusPreview); e.Handled = true; }

        else if (e.Key == Key.Escape && _focusPreview) { SetPreviewFocus(false); e.Handled = true; }

        else if (e.Key == Key.F1) { HelpClick(sender, e); e.Handled = true; }

        else if (e.Key == Key.OemComma && Keyboard.Modifiers == ModifierKeys.Control) { ConnectionClick(sender, e); e.Handled = true; }
    }

    private void TitleDrag(object sender, MouseButtonEventArgs e)
    {
        if (e.ClickCount == 2)
        {
            MaximizeClick(sender, e);
        }
        else if (e.LeftButton == MouseButtonState.Pressed)
        {
            DragMove();
        }
    }

    protected override void OnSourceInitialized(EventArgs e)
    {
        base.OnSourceInitialized(e);
        try
        {
            var handle = new System.Windows.Interop.WindowInteropHelper(this).Handle;
            if (handle != IntPtr.Zero)
            {
                int darkMode = 1;
                DwmSetWindowAttribute(handle, DWMWA_USE_IMMERSIVE_DARK_MODE, ref darkMode, sizeof(int));
            }
        }
        catch { }
        System.Windows.Interop.HwndSource.FromHwnd(new System.Windows.Interop.WindowInteropHelper(this).Handle)?.AddHook(WindowBoundsHook);
    }
    private static nint WindowBoundsHook(nint hwnd, int message, nint wParam, nint lParam, ref bool handled)
    {
        if (message != 0x24) return 0;
        var monitor = MonitorFromWindow(hwnd, 2);
        var info = new MonitorInfo { Size = Marshal.SizeOf<MonitorInfo>() };
        if (GetMonitorInfo(monitor, ref info))
        {
            var limits = Marshal.PtrToStructure<MinMaxInfo>(lParam);
            limits.MaxPosition = new NativePoint { X = info.Work.Left - info.Monitor.Left, Y = info.Work.Top - info.Monitor.Top };
            limits.MaxSize = new NativePoint { X = info.Work.Right - info.Work.Left, Y = info.Work.Bottom - info.Work.Top };
            Marshal.StructureToPtr(limits, lParam, false);
            handled = true;
        }
        return 0;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct NativePoint
    {
        public int X, Y;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct NativeRect
    {
        public int Left, Top, Right, Bottom;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct MinMaxInfo
    {
        public NativePoint Reserved, MaxSize, MaxPosition, MinTrack, MaxTrack;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct MonitorInfo
    {
        public int Size;
        public NativeRect Monitor, Work;
        public uint Flags;
    }
    [DllImport("user32.dll")] private static extern nint MonitorFromWindow(nint hwnd, uint flags);
    [DllImport("user32.dll", CharSet = CharSet.Auto)] private static extern bool GetMonitorInfo(nint monitor, ref MonitorInfo info);
    private void MinimizeClick(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;

    private void MaximizeClick(object sender, RoutedEventArgs e) => WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;

    private void CloseClick(object sender, RoutedEventArgs e) => Close();

    private void BackgroundCard_Drop(object sender, DragEventArgs e)
    {
        if (!e.Data.GetDataPresent(DataFormats.FileDrop)) return;
        var files = (string[]?)e.Data.GetData(DataFormats.FileDrop);
        if (files == null || files.Length == 0) return;
        var path = files[0];
        if (!File.Exists(path)) return;
        var ext = Path.GetExtension(path).ToLowerInvariant();
        bool isVideo = ext is ".mp4" or ".mkv" or ".mov";
        bool isImage = ext is ".png" or ".jpg" or ".jpeg" or ".bmp" or ".tif" or ".tiff" or ".webp";
        if (!isVideo && !isImage) return;
        PreviewHost.SetPath(isVideo ? 2 : 1, path);
        UpdateBackgroundCard(path, isVideo);
        if (_state.Mode == 0 || (_state.Packets == 0 && _state.Mode != 3))
        {
            var targetMode = _state.Packets == 0 ? 3 : 2; PreviewHost.Set(2, targetMode); SelectMode(targetMode);
        }
    }

    private async void OnClosing(object? sender, CancelEventArgs e)

    {
        if (_closed) return;

        e.Cancel = true;
        if (_closing) return;

        _closing = true; _timer.Stop(); RunButton.IsEnabled = false; RunButton.ToolTip = "Closing…";

        await Task.Run(PreviewHost.StopEngine);

        _installer?.Dispose(); _networkSetup?.Dispose(); _closed = true; Close();
    }
}

