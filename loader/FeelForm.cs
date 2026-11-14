// ryGT7 launcher: the HD rumble & adaptive triggers window (FEAT-GTS-265, from RyGTS FEAT-GTS-280). One slider per effect,
// 0-200% (0 turns that effect off), saved to the game config's Input object as it moves and read
// by the renderer (shadgt7 FEAT-GT7-179) every second, so changes apply while playing. Each effect has a Test button that
// plays it on the DualSense through tools\ryty-feel-test.exe, which uses the renderer's own effect
// code, so a test feels the same as on track.
// SPDX-License-Identifier: GPL-2.0-only

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Windows.Forms;

namespace RyGT
{
    class FeelForm : Form
    {
        /// One setting: its key in the config (and ryty-feel-test's argument name), its label, and
        /// the effect its Test button plays (null: no Test button).
        class Item
        {
            public string Key, Label, Test;
            public Item(string key, string label, string test) { Key = key; Label = label; Test = test; }
        }

        static readonly Item[] Rumble =
        {
            new Item("haptics_strength", "Overall HD rumble strength", null),
            new Item("haptics_engine", "Engine: a hum that follows the revs", "engine"),
            new Item("haptics_road", "Road: kerbs and bumps, on their side", "road"),
            new Item("haptics_slip", "Wheel lock and wheelspin", "slip"),
            new Item("haptics_shift", "Gear changes", "shift"),
            new Item("haptics_limiter", "Rev limiter", "limiter"),
            new Item("haptics_impact", "Crashes and hard hits", "impact"),
        };

        static readonly Item[] Triggers =
        {
            new Item("trigger_strength", "Overall trigger strength", null),
            new Item("trigger_brake", "Brake resistance (L2)", "brake"),
            new Item("trigger_throttle", "Accelerator resistance (R2)", "throttle"),
            new Item("trigger_dynamic", "Resistance follows speed, braking and revs", "brake"),
            new Item("trigger_lock", "ABS: wheel-lock vibration (L2)", "lock"),
            new Item("trigger_spin", "Wheelspin vibration (R2)", "spin"),
        };

        readonly string helperPath;
        readonly Action<string, int> write;
        readonly Dictionary<string, Slider> sliders = new Dictionary<string, Slider>();
        readonly Label status;
        Process test;

        public FeelForm(string root, Func<string, int, int> read, Action<string, int> write)
        {
            this.write = write;
            helperPath = Path.Combine(root, "tools", "ryty-feel-test.exe");
            if (!File.Exists(helperPath)) helperPath = Path.Combine(root, "ryty-feel-test.exe");

            Text = "ryGT - HD rumble & adaptive triggers";
            Font = new Font("Segoe UI", 9f);
            StartPosition = FormStartPosition.CenterParent;
            ClientSize = new Size(780, 820);
            MinimumSize = new Size(640, 520);
            FormBorderStyle = FormBorderStyle.Sizable;
            MinimizeBox = false;
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }

            var outer = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, Padding = new Padding(18, 14, 18, 0) };
            outer.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            Controls.Add(outer);
            outer.Controls.Add(new Label { Text = "HD rumble & adaptive triggers", UseMnemonic = false, AutoSize = true, Font = new Font("Segoe UI Semibold", 16f), Margin = new Padding(0, 0, 0, 4) });
            outer.Controls.Add(new Label
            {
                AutoSize = true, MaximumSize = new Size(720, 0), Margin = new Padding(0, 0, 0, 8),
                ForeColor = Dark.DimText, Tag = "heading",
                Text = "How the DualSense feels the car. 100% is standard, 200% boosted, and 0% turns that effect off. " +
                       "Changes are saved at once and apply while playing. Test plays the effect on your controller."
            });

            var list = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, AutoScroll = true, Margin = new Padding(0) };
            list.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            outer.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            outer.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            outer.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            outer.Controls.Add(list);
            list.Controls.Add(Section("HD rumble",
                "On the controller's body, on top of its normal rumble, from the game's telemetry. It needs the " +
                "DualSense connected by USB cable.", Rumble, read));
            list.Controls.Add(Section("Adaptive triggers",
                "Pedal feel on L2 and R2. With ABS or wheelspin at 0%, the pedal keeps its resistance instead of vibrating.",
                Triggers, read));

            // Status and the buttons stay at the bottom, below the scrolling list.
            status = new Label { AutoSize = true, MaximumSize = new Size(720, 0), ForeColor = Dark.DimText, Margin = new Padding(0, 6, 0, 4) };
            outer.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            outer.Controls.Add(status);
            var actions = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Fill, FlowDirection = FlowDirection.RightToLeft, WrapContents = false, Margin = new Padding(0, 0, 0, 12) };
            outer.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            outer.Controls.Add(actions);
            var close = new Button { Text = "Close", AutoSize = true, Tag = "accent", DialogResult = DialogResult.Cancel };
            var reset = new Button { Text = "Reset all to 100%", AutoSize = true };
            reset.Click += delegate { foreach (var slider in sliders.Values) slider.Value = 100; };
            var stop = new Button { Text = "Stop test", AutoSize = true };
            stop.Click += delegate { StopTest(); status.Text = ""; };
            actions.Controls.Add(close);
            actions.Controls.Add(reset);
            actions.Controls.Add(stop);
            CancelButton = close;

            if (!File.Exists(helperPath))
            {
                status.Text = "tools\\ryty-feel-test.exe is missing, so the Test buttons cannot play anything. " +
                              "The settings still apply in game.";
            }
            FormClosing += delegate { StopTest(); };
            Dark.Apply(this);
            HandleCreated += delegate { Dark.TitleBar(this); };
        }

        Control Section(string title, string hint, Item[] items, Func<string, int, int> read)
        {
            var card = new Card(Dark.Panel) { AutoSize = true, Dock = DockStyle.Top, Padding = new Padding(16, 10, 16, 12), Margin = new Padding(0, 0, 0, 10) };
            card.Controls.Add(new Label { Text = Dark.Spaced(title.ToUpperInvariant()), AutoSize = true, Font = Dark.Caps(9f), ForeColor = Dark.Heading, Tag = "heading", Margin = new Padding(0, 0, 0, 2) });
            card.Controls.Add(new Label { Text = hint, AutoSize = true, MaximumSize = new Size(680, 0), ForeColor = Dark.DimText, Tag = "heading", Margin = new Padding(0, 0, 0, 6) });
            foreach (var entry in items)
            {
                var item = entry;
                var slider = new Slider
                {
                    Minimum = 0, Maximum = 200, SmallChange = 5, LargeChange = 25, Width = 300, Suffix = "%",
                    AccessibleName = item.Label
                };
                slider.Value = Math.Max(0, Math.Min(200, read(item.Key, 100)));
                slider.ValueChanged += delegate
                {
                    try { write(item.Key, slider.Value); }
                    catch (Exception e) { MessageBox.Show(this, "Could not save " + item.Label + ": " + e.Message); }
                };
                sliders[item.Key] = slider;
                var controls = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0), BackColor = Color.Transparent };
                controls.Controls.Add(slider);
                var testButton = new Button { Text = "Test", AutoSize = true, Margin = new Padding(8, 2, 0, 0), Enabled = item.Test != null };
                if (item.Test == null) testButton.Visible = false;
                testButton.Click += delegate { StartTest(item.Test, item.Label); };
                controls.Controls.Add(testButton);
                card.Controls.Add(new SettingRow(item.Label, controls));
            }
            return card;
        }

        void StartTest(string effect, string label)
        {
            StopTest();
            if (!File.Exists(helperPath))
            {
                status.ForeColor = Dark.AccentText;
                status.Text = "tools\\ryty-feel-test.exe is missing, so the Test buttons cannot play anything.";
                return;
            }
            var arguments = effect + " " + string.Join(" ", sliders.Select(pair => pair.Key + "=" + pair.Value.Value).ToArray());
            var info = new ProcessStartInfo(helperPath, arguments)
            {
                // A line on its input stops it early and releases the triggers (StopTest).
                UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardInput = true, CreateNoWindow = true
            };
            status.ForeColor = Dark.DimText;
            status.Text = "Testing " + label + "...";
            try
            {
                var process = new Process { StartInfo = info, EnableRaisingEvents = true };
                process.OutputDataReceived += (sender, e) => { if (e.Data != null) Show(e.Data); };
                process.Exited += delegate { Show(null); };
                process.Start();
                process.BeginOutputReadLine();
                test = process;
            }
            catch (Exception e)
            {
                status.ForeColor = Dark.AccentText;
                status.Text = "Could not start the test: " + e.Message;
            }
        }

        /// A line from ryty-feel-test ("phase: ...", "error: ..."), or null when it has exited.
        void Show(string line)
        {
            if (IsDisposed || !IsHandleCreated) return;
            try { BeginInvoke((Action)delegate { ShowNow(line); }); } catch (InvalidOperationException) { }
        }

        void ShowNow(string line)
        {
            {
                if (line == null) return;
                if (line.StartsWith("error: ", StringComparison.Ordinal))
                {
                    status.ForeColor = Dark.AccentText;
                    status.Text = line.Substring(7);
                }
                else if (line == "phase: done")
                {
                    status.ForeColor = Dark.DimText;
                    status.Text = "Test finished.";
                }
                else if (line.StartsWith("phase: ", StringComparison.Ordinal))
                {
                    status.ForeColor = Dark.Text;
                    status.Text = "Playing " + line.Substring(7);
                }
            }
        }

        void StopTest()
        {
            var process = test;
            test = null;
            if (process == null) return;
            // Asked to stop, it releases the triggers and fades the rumble; killed, the last trigger
            // effect would stay on the controller.
            try
            {
                if (!process.HasExited)
                {
                    process.StandardInput.WriteLine("stop");
                    process.StandardInput.Close();
                    if (!process.WaitForExit(1500)) process.Kill();
                }
            }
            catch { try { if (!process.HasExited) process.Kill(); } catch { } }
            process.Dispose();
        }
    }
}
