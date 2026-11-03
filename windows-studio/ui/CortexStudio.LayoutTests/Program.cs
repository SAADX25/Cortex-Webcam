using System.Reflection;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using CortexStudio;

internal static class Program
{
    private static int _checks;
    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
        _checks++;
    }

    [STAThread]
    private static int Main()
    {
        try
        {
            var app = new App();
            app.InitializeComponent();
            var window = new MainWindow();
            Check(Marshal.SizeOf(typeof(MainWindow).Assembly.GetType("CortexStudio.NativeState")!) == 3576, "Managed/native FPS state ABI must match");
            Check(window.FindName("FpsSyncStatus") is TextBlock, "Frame-rate synchronization feedback is missing");
            var avatar = (CheckBox)window.FindName("AvatarEnabled");
            var motion = (CheckBox)window.FindName("AvatarMotion");
            Check(avatar.IsChecked != true && !motion.IsEnabled, "Avatar must default to off");
            avatar.IsChecked = true;
            Check(motion.IsEnabled, "Test motion must become available with avatar mode");
            avatar.IsChecked = false;
            var testBtn = (Button)window.FindName("AvatarTestBtn");
            var cubismBtn = (Button)window.FindName("AvatarCubismBtn");
            var modelCard = (FrameworkElement)window.FindName("CubismModelCard");
            var detailsPanel = (FrameworkElement)window.FindName("AvatarDetailsPanel");
            Check(testBtn is not null && cubismBtn is not null && modelCard is not null, "Avatar model controls must be present");
            var root = (FrameworkElement)window.Content;
            var workspace = (Grid)window.FindName("WorkspaceGrid");
            var inspector = (FrameworkElement)window.FindName("InspectorPanel");
            var preview = (FrameworkElement)window.FindName("PreviewHost");
            var tabs = (TabControl)window.FindName("InspectorTabs");
            var focus = typeof(MainWindow).GetMethod("SetPreviewFocus", BindingFlags.NonPublic | BindingFlags.Instance)!;

            foreach (var size in new[] { new Size(1080, 720), new Size(1280, 800), new Size(1440, 900), new Size(1920, 1080) })
            {
                void Layout()
                {
                    root.Measure(size);
                    root.Arrange(new Rect(size));
                    root.UpdateLayout();
                }
                Layout();
                Check(preview.ActualWidth >= 400 && preview.ActualHeight >= 220, $"Preview too small at {size}");
                var inspectorPosition = inspector.TranslatePoint(new Point(), root);
                Check(inspectorPosition.X >= 0 && inspectorPosition.X + inspector.ActualWidth <= size.Width, $"Inspector clipped at {size}");
                var oldWidth = preview.ActualWidth;
                tabs.SelectedIndex = 1;
                Layout();
                Check(((FrameworkElement)window.FindName("OutputToggle")).ActualWidth > 100, $"Output controls are not usable at {size}");
                focus.Invoke(window, new object[] { true });
                Layout();
                Check(preview.ActualWidth > oldWidth + 400, $"Focus preview does not expand at {size}");
                focus.Invoke(window, new object[] { false });
                Layout();
                Check(Math.Abs(preview.ActualWidth - oldWidth) < 1 && tabs.SelectedIndex == 1, $"Focus mode loses layout/tab state at {size}");
                tabs.SelectedIndex = 0;
                Layout();
                Check(((Button)window.FindName("AiButton")).ActualWidth > 200, $"Appearance controls clipped at {size}");
                Console.WriteLine($"Layout and focus passed: {size.Width} x {size.Height}");
            }
            Check(workspace.ColumnDefinitions.Count == 3, "Workspace column contract changed");
            Console.WriteLine($"PASS: {_checks} layout checks. No camera stream or external app was opened.");
            app.Shutdown();
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error);
            return 1;
        }
    }
}
