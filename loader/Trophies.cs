// SPDX-License-Identifier: GPL-2.0-only
// FEAT-GT7-187: the launcher's Trophies page. The player's own 32-hex trophy key (kept with
// Windows DPAPI in user\trophy.key.dpapi, never in plain text and never logged), a Test of it
// against the game's sce_sys\trophy\trophy00.trp, the trophy list with what is earned (read from
// the game's per-user unlock file), the pop-up switch and the Trophy-Notifier asset import.
// At Play the key goes to the game through a short-lived file named by RYGT_TROPHY_KEY_FILE.

using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Security.AccessControl;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows.Forms;
using System.Xml;

namespace RyGT
{
    /// One file inside a TRP archive.
    class TrpEntry
    {
        public string Name;
        public long Offset, Length;
        public uint Flag;
    }

    /// One trophy from TROP.ESFM.
    class TrophyInfo
    {
        public int Id;
        public string IdText = "", Name = "", Detail = "";
        public char Grade = 'B';
        public bool Hidden, Earned;
        public DateTime? Unlocked;
    }

    /// The TRP / ESFM format and its decryption (no files, no UI: the tests use it directly).
    static class TrophyCrypto
    {
        public const uint TrpMagic = 0xDCA24D00;
        public const int HeaderSize = 0x60, EntrySize = 0x40;
        static readonly Regex HexKey = new Regex("^[0-9A-Fa-f]{32}$");
        public static readonly Regex NpCommIdPattern = new Regex("NPWR[0-9]{5}_[0-9]{2}");

        public static bool ValidKey(string hex) { return hex != null && HexKey.IsMatch(hex); }

        public static string CleanKey(string text)
        {
            return new string((text ?? "").Where(c => !char.IsWhiteSpace(c)).ToArray());
        }

        public static byte[] HexToBytes(string hex)
        {
            var bytes = new byte[hex.Length / 2];
            for (int i = 0; i < bytes.Length; ++i)
                bytes[i] = byte.Parse(hex.Substring(i * 2, 2), NumberStyles.HexNumber, CultureInfo.InvariantCulture);
            return bytes;
        }

        static uint U32(byte[] b, long at) { return (uint)(b[at] << 24 | b[at + 1] << 16 | b[at + 2] << 8 | b[at + 3]); }
        static long U64(byte[] b, long at) { return (long)((ulong)U32(b, at) << 32 | U32(b, at + 4)); }

        /// The entry table of a TRP (big-endian; header magic 0xDCA24D00). Throws on a bad file.
        public static List<TrpEntry> ParseTrp(byte[] trp)
        {
            if (trp == null || trp.Length < HeaderSize) throw new InvalidDataException("The trophy file is too short.");
            if (U32(trp, 0) != TrpMagic) throw new InvalidDataException("The trophy file is not a TRP (bad magic).");
            uint count = U32(trp, 0x10), size = U32(trp, 0x14);
            if (size < EntrySize || count > 4096 || HeaderSize + (long)count * size > trp.Length)
                throw new InvalidDataException("The trophy file's entry table is damaged.");
            var list = new List<TrpEntry>();
            for (long i = 0; i < count; ++i)
            {
                long at = HeaderSize + i * size;
                int end = 0;
                while (end < 32 && trp[at + end] != 0) ++end;
                var entry = new TrpEntry
                {
                    Name = Encoding.ASCII.GetString(trp, (int)at, end),
                    Offset = U64(trp, at + 0x20), Length = U64(trp, at + 0x28), Flag = U32(trp, at + 0x30)
                };
                if (entry.Offset < 0 || entry.Length < 0 || entry.Offset + entry.Length > trp.Length)
                    throw new InvalidDataException("The trophy file's entry " + entry.Name + " points outside the file.");
                list.Add(entry);
            }
            return list;
        }

        public static byte[] EntryBytes(byte[] trp, TrpEntry entry)
        {
            var data = new byte[entry.Length];
            Buffer.BlockCopy(trp, (int)entry.Offset, data, 0, data.Length);
            return data;
        }

        static RijndaelManaged Aes128(byte[] key, byte[] iv)
        {
            return new RijndaelManaged { BlockSize = 128, KeySize = 128, Mode = CipherMode.CBC, Padding = PaddingMode.None, Key = key, IV = iv };
        }

        /// trpKey = AES-128-CBC-Encrypt(key, IV 0, npCommId padded to 16 bytes with zeros).
        public static byte[] DeriveTrpKey(byte[] key, string npCommId)
        {
            var block = new byte[16];
            var id = Encoding.ASCII.GetBytes(npCommId ?? "");
            Buffer.BlockCopy(id, 0, block, 0, Math.Min(16, id.Length));
            using (var aes = Aes128(key, new byte[16]))
            using (var enc = aes.CreateEncryptor())
                return enc.TransformFinalBlock(block, 0, 16);
        }

        /// An encrypted ESFM entry (flag 3): its first 16 bytes are the IV, the rest AES-128-CBC
        /// with trpKey. Returns the XML text, cut after its last '>' (the padding goes).
        public static string DecryptEsfm(byte[] trpKey, byte[] entry)
        {
            if (entry == null || entry.Length < 32) throw new InvalidDataException("The trophy entry is too short.");
            var iv = new byte[16];
            Buffer.BlockCopy(entry, 0, iv, 0, 16);
            int length = (entry.Length - 16) / 16 * 16;
            byte[] plain;
            using (var aes = Aes128(trpKey, iv))
            using (var dec = aes.CreateDecryptor())
                plain = dec.TransformFinalBlock(entry, 16, length);
            string text = Encoding.UTF8.GetString(plain);
            int last = text.LastIndexOf('>');
            return last >= 0 ? text.Substring(0, last + 1) : "";
        }

        /// The test's encryption, the reverse of DecryptEsfm (synthetic files in the tests).
        public static byte[] EncryptEsfm(byte[] trpKey, byte[] iv, string xml)
        {
            var data = Encoding.UTF8.GetBytes(xml);
            var padded = new byte[(data.Length + 15) / 16 * 16];
            Buffer.BlockCopy(data, 0, padded, 0, data.Length);
            using (var aes = Aes128(trpKey, iv))
            using (var enc = aes.CreateEncryptor())
            {
                var cipher = enc.TransformFinalBlock(padded, 0, padded.Length);
                var result = new byte[16 + cipher.Length];
                Buffer.BlockCopy(iv, 0, result, 0, 16);
                Buffer.BlockCopy(cipher, 0, result, 16, cipher.Length);
                return result;
            }
        }

        /// The decrypted text is a trophy configuration: an optional BOM and <?xml ?> declaration, Sony's
        /// <!--Sce-Np-Trophy-Signature: ...--> comment, then <trophyconf. As a fallback, <trophyconf anywhere
        /// in the first 4 KB counts; a wrong key decrypts to random bytes, which never contain it.
        public static bool LooksLikeTrophyConf(string xml)
        {
            if (string.IsNullOrEmpty(xml)) return false;
            string head = xml.TrimStart('\uFEFF');
            for (;;)
            {
                head = head.TrimStart(' ', '\r', '\n', '\t');
                string close = head.StartsWith("<?", StringComparison.Ordinal) ? "?>" :
                               head.StartsWith("<!--", StringComparison.Ordinal) ? "-->" : null;
                if (close == null) break;
                int end = head.IndexOf(close, StringComparison.Ordinal);
                if (end < 0) break;
                head = head.Substring(end + close.Length);
            }
            if (head.StartsWith("<trophyconf", StringComparison.Ordinal)) return true;
            return xml.Substring(0, Math.Min(xml.Length, 4096)).IndexOf("<trophyconf", StringComparison.Ordinal) >= 0;
        }

        /// What a failed decrypt looks like, for the Test message: unreadable bytes (wrong key or npCommId)
        /// or readable XML whose first element is shown. Never includes the key.
        public static string Describe(string text)
        {
            if (string.IsNullOrEmpty(text)) return "nothing readable";
            string head = text.Substring(0, Math.Min(text.Length, 512));
            int printable = head.Count(c => c == '\r' || c == '\n' || c == '\t' || (c >= ' ' && c < 0x7f));
            if (printable < head.Length * 9 / 10) return "unreadable bytes";
            var tag = Regex.Match(head, "<([A-Za-z][A-Za-z0-9_-]*)");
            return tag.Success ? "XML starting with <" + tag.Groups[1].Value + ">" : "text without XML";
        }

        public static TrpEntry Find(List<TrpEntry> entries, string name)
        {
            return entries.FirstOrDefault(e => e.Name.Equals(name, StringComparison.OrdinalIgnoreCase));
        }

        /// Decrypts a named ESFM entry; null when it is missing.
        public static string DecryptEntry(byte[] trp, List<TrpEntry> entries, string name, byte[] key, string npCommId)
        {
            var entry = Find(entries, name);
            if (entry == null) return null;
            if (entry.Flag != 3)
            {
                // A plain (unencrypted) entry.
                string text = Encoding.UTF8.GetString(EntryBytes(trp, entry));
                int last = text.LastIndexOf('>');
                return last >= 0 ? text.Substring(0, last + 1) : "";
            }
            return DecryptEsfm(DeriveTrpKey(key, npCommId), EntryBytes(trp, entry));
        }

        /// The trophies in TROP.ESFM's XML.
        public static List<TrophyInfo> ParseTrophies(string xml)
        {
            var doc = new XmlDocument { XmlResolver = null };
            int start = xml.IndexOf('<');
            doc.LoadXml(start > 0 ? xml.Substring(start) : xml);
            var list = new List<TrophyInfo>();
            foreach (XmlElement node in doc.GetElementsByTagName("trophy"))
            {
                var t = new TrophyInfo { IdText = node.GetAttribute("id") };
                int.TryParse(t.IdText, NumberStyles.Integer, CultureInfo.InvariantCulture, out t.Id);
                t.Hidden = node.GetAttribute("hidden").Equals("yes", StringComparison.OrdinalIgnoreCase);
                string grade = node.GetAttribute("ttype").ToUpperInvariant();
                t.Grade = grade.Length == 1 && "BSGP".IndexOf(grade[0]) >= 0 ? grade[0] : 'B';
                var name = node["name"];
                var detail = node["detail"];
                t.Name = name != null ? name.InnerText.Trim() : "";
                t.Detail = detail != null ? detail.InnerText.Trim() : "";
                list.Add(t);
            }
            return list;
        }

        static readonly Regex TrophyTag = new Regex("<trophy\\b[^>]*>", RegexOptions.IgnoreCase);

        static string Attribute(string tag, string name)
        {
            var m = Regex.Match(tag, "\\b" + name + "\\s*=\\s*\"([^\"]*)\"", RegexOptions.IgnoreCase);
            return m.Success ? m.Groups[1].Value : null;
        }

        /// The per-user unlock file (shadPS4's TROP.XML copy): id -> unlock time (null when unknown).
        public static Dictionary<int, DateTime?> ParseUnlocks(string xml)
        {
            var earned = new Dictionary<int, DateTime?>();
            foreach (Match m in TrophyTag.Matches(xml ?? ""))
            {
                string state = Attribute(m.Value, "unlockstate"), id = Attribute(m.Value, "id");
                int number;
                if (state == null || !state.Equals("true", StringComparison.OrdinalIgnoreCase) || id == null ||
                    !int.TryParse(id, NumberStyles.Integer, CultureInfo.InvariantCulture, out number))
                    continue;
                earned[number] = Timestamp(Attribute(m.Value, "timestamp"));
            }
            return earned;
        }

        /// A timestamp as the runtimes write it: Unix seconds, milliseconds or microseconds, or
        /// sceRtc ticks (microseconds since 0001-01-01). Local time.
        public static DateTime? Timestamp(string text)
        {
            long value;
            if (text == null || !long.TryParse(text.Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out value) || value <= 0)
                return null;
            try
            {
                var epoch = new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc);
                DateTime utc;
                if (value >= 30000000000000000L) utc = new DateTime(value * 10, DateTimeKind.Utc);  // Rtc ticks (µs since year 1)
                else if (value >= 100000000000000L) utc = epoch.AddTicks(value * 10);               // Unix µs
                else if (value >= 100000000000L) utc = epoch.AddMilliseconds(value);                // Unix ms
                else utc = epoch.AddSeconds(value);                                                 // Unix s
                return utc.ToLocalTime();
            }
            catch { return null; }
        }
    }

    /// The trophy files and the key on disk.
    static class TrophyStore
    {
        static readonly byte[] Entropy = Encoding.ASCII.GetBytes("ryGT7 trophy key FEAT-GT7-187");

        public static string UserDir(string root) { return Path.Combine(root, "user"); }
        public static string KeyPath(string root) { return Path.Combine(UserDir(root), "trophy.key.dpapi"); }
        static string PendingPath(string root) { return Path.Combine(UserDir(root), "trophy.keyfile.pending"); }
        static string IniPath(string root) { return Path.Combine(UserDir(root), "trophy.ini"); }
        public static string AssetsDir(string root) { return Path.Combine(UserDir(root), "trophy_assets"); }

        public static bool HasKey(string root) { return File.Exists(KeyPath(root)); }

        public static void SaveKey(string root, string hex)
        {
            if (!TrophyCrypto.ValidKey(hex)) throw new ArgumentException("The trophy key is 32 hexadecimal characters.");
            byte[] plain = Encoding.ASCII.GetBytes(hex);
            byte[] sealedKey = ProtectedData.Protect(plain, Entropy, DataProtectionScope.CurrentUser);
            Array.Clear(plain, 0, plain.Length);
            Directory.CreateDirectory(UserDir(root));
            string temp = KeyPath(root) + ".new";
            File.WriteAllBytes(temp, sealedKey);
            if (File.Exists(KeyPath(root))) File.Delete(KeyPath(root));
            File.Move(temp, KeyPath(root));
        }

        /// The stored key, or null (none, or not this Windows user's).
        public static string LoadKey(string root)
        {
            try
            {
                if (!HasKey(root)) return null;
                byte[] plain = ProtectedData.Unprotect(File.ReadAllBytes(KeyPath(root)), Entropy, DataProtectionScope.CurrentUser);
                string hex = Encoding.ASCII.GetString(plain);
                Array.Clear(plain, 0, plain.Length);
                return TrophyCrypto.ValidKey(hex) ? hex : null;
            }
            catch { return null; }
        }

        public static void DeleteKey(string root)
        {
            if (File.Exists(KeyPath(root))) File.Delete(KeyPath(root));
        }

        // ---- the pop-up switch (user\trophy.ini, toast=0 turns them off)
        public static bool ToastsOn(string root)
        {
            try
            {
                if (!File.Exists(IniPath(root))) return true;
                foreach (var line in File.ReadAllLines(IniPath(root)))
                    if (line.Trim().Replace(" ", "") == "toast=0") return false;
            }
            catch { }
            return true;
        }

        public static void SetToasts(string root, bool on)
        {
            Directory.CreateDirectory(UserDir(root));
            var lines = File.Exists(IniPath(root)) ? File.ReadAllLines(IniPath(root)).ToList() : new List<string>();
            lines.RemoveAll(l => l.TrimStart().StartsWith("toast=", StringComparison.OrdinalIgnoreCase));
            lines.Add("toast=" + (on ? "1" : "0"));
            File.WriteAllLines(IniPath(root), lines);
        }

        // ---- the key file for one game session
        /// Writes the key to a new temp file only this Windows user can read and returns its path
        /// (null without a stored key). The pending note holds the path (never the key) so a file a
        /// closed launcher left is deleted at the next start.
        public static string PrepareKeyFile(string root)
        {
            string hex = LoadKey(root);
            if (hex == null) return null;
            string path = Path.GetTempFileName();
            try
            {
                try
                {
                    var security = new FileSecurity();
                    security.SetAccessRuleProtection(true, false);
                    security.AddAccessRule(new FileSystemAccessRule(WindowsIdentity.GetCurrent().User, FileSystemRights.FullControl, AccessControlType.Allow));
                    File.SetAccessControl(path, security);
                }
                catch { }
                try { File.WriteAllText(PendingPath(root), path); } catch { }
                File.WriteAllText(path, hex, Encoding.ASCII);
                return path;
            }
            catch
            {
                DeleteKeyFile(root, path);
                return null;
            }
        }

        public static void DeleteKeyFile(string root, string path)
        {
            if (path == null) return;
            try
            {
                if (File.Exists(path))
                {
                    // Overwrite before deleting, so the key is not left in the freed clusters.
                    try { File.WriteAllBytes(path, new byte[32]); } catch { }
                    File.Delete(path);
                }
            }
            catch { }
            try
            {
                if (File.Exists(PendingPath(root)) && File.ReadAllText(PendingPath(root)).Trim() == path)
                    File.Delete(PendingPath(root));
            }
            catch { }
        }

        /// At launcher start: a key file left by a launcher closed while GT7 ran goes.
        public static void CleanUpLeftoverKeyFile(string root)
        {
            try
            {
                if (!File.Exists(PendingPath(root))) return;
                string path = File.ReadAllText(PendingPath(root)).Trim();
                string temp = Path.GetFullPath(Path.GetTempPath());
                if (path.Length > 0 && Path.GetFullPath(path).StartsWith(temp, StringComparison.OrdinalIgnoreCase) &&
                    File.Exists(path) && new FileInfo(path).Length <= 64)
                {
                    try { File.WriteAllBytes(path, new byte[32]); } catch { }
                    File.Delete(path);
                }
                File.Delete(PendingPath(root));
            }
            catch { }
        }

        /// Sets the game's trophy variables; returns the key file to delete after the game exits.
        public static string ApplyEnvironment(string root, System.Diagnostics.ProcessStartInfo start)
        {
            string file = null;
            try { file = PrepareKeyFile(root); } catch { }
            if (file != null) start.EnvironmentVariables["RYGT_TROPHY_KEY_FILE"] = file;
            else start.EnvironmentVariables.Remove("RYGT_TROPHY_KEY_FILE");
            if (!ToastsOn(root)) start.EnvironmentVariables["RYGT_TROPHY_TOAST"] = "0";
            return file;
        }

        // ---- the game's files
        /// The app0 layers the game reads, top first: ryGT7\app0, then RYGT_APP0_LAYERS (lowest first,
        /// when set) or the update= and game= folders of RyGT.ini.
        public static List<string> App0Layers(string root, string update, string game)
        {
            var folders = new List<string> { Path.Combine(root, "app0") };
            string env = Environment.GetEnvironmentVariable("RYGT_APP0_LAYERS");
            if (!string.IsNullOrEmpty(env))
            {
                var layers = env.Split(';').Select(s => s.Trim()).Where(s => s.Length > 0).ToList();
                layers.Reverse();
                folders.AddRange(layers);
                return folders;
            }
            string ini = Path.Combine(root, "RyGT.ini");
            try
            {
                if (File.Exists(ini))
                    foreach (var line in File.ReadAllLines(ini))
                    {
                        int eq = line.IndexOf('=');
                        if (eq < 0) continue;
                        string key = line.Substring(0, eq).Trim(), value = line.Substring(eq + 1).Trim();
                        if (key == "update" && value.Length > 0) update = value;
                        else if (key == "game" && value.Length > 0) game = value;
                    }
            }
            catch { }
            if (!string.IsNullOrEmpty(update)) folders.Add(update);
            if (!string.IsNullOrEmpty(game)) folders.Add(game);
            return folders;
        }

        public static string FindGameFile(List<string> layers, string relative)
        {
            foreach (var folder in layers)
            {
                try
                {
                    string path = Path.Combine(folder, relative);
                    if (File.Exists(path)) return path;
                }
                catch { }
            }
            return null;
        }

        /// npCommId candidates: npbind.dat, then the trophy folders and unlock files already there.
        public static List<string> NpCommIds(string root, List<string> layers)
        {
            var ids = new List<string>();
            string npbind = FindGameFile(layers, Path.Combine("sce_sys", "npbind.dat"));
            try
            {
                if (npbind != null)
                    foreach (Match m in TrophyCrypto.NpCommIdPattern.Matches(Encoding.ASCII.GetString(File.ReadAllBytes(npbind))))
                        if (!ids.Contains(m.Value)) ids.Add(m.Value);
            }
            catch { }
            try
            {
                string trophy = Path.Combine(UserDir(root), "trophy");
                if (Directory.Exists(trophy))
                    foreach (var dir in Directory.GetDirectories(trophy))
                    {
                        var m = TrophyCrypto.NpCommIdPattern.Match(Path.GetFileName(dir));
                        if (m.Success && !ids.Contains(m.Value)) ids.Add(m.Value);
                    }
                foreach (var file in UnlockFiles(root, null))
                {
                    var m = TrophyCrypto.NpCommIdPattern.Match(Path.GetFileName(file));
                    if (m.Success && !ids.Contains(m.Value)) ids.Add(m.Value);
                }
            }
            catch { }
            return ids;
        }

        /// The per-user unlock files: home\1000 first, then any other home\*\trophy (npCommId null: all).
        public static List<string> UnlockFiles(string root, string npCommId)
        {
            var list = new List<string>();
            string home = Path.Combine(UserDir(root), "home");
            if (!Directory.Exists(home)) return list;
            var users = new List<string>();
            string first = Path.Combine(home, "1000");
            if (Directory.Exists(first)) users.Add(first);
            try { users.AddRange(Directory.GetDirectories(home).Where(d => !d.Equals(first, StringComparison.OrdinalIgnoreCase)).OrderBy(d => d)); }
            catch { }
            foreach (var user in users)
            {
                string dir = Path.Combine(user, "trophy");
                try
                {
                    if (npCommId != null)
                    {
                        string file = Path.Combine(dir, npCommId + ".xml");
                        if (File.Exists(file)) list.Add(file);
                    }
                    else if (Directory.Exists(dir)) list.AddRange(Directory.GetFiles(dir, "NPWR*.xml"));
                }
                catch { }
            }
            return list;
        }

        // ---- Trophy-Notifier's assets
        static readonly string[] NotifierNames = { "TrophyNotifier", "Trophy-Notifier", "Trophy Notifier" };

        static string AssetsRootOf(string folder)
        {
            if (string.IsNullOrEmpty(folder)) return null;
            try
            {
                if (Path.GetFileName(folder.TrimEnd('\\', '/')).Equals("assets", StringComparison.OrdinalIgnoreCase) && Directory.Exists(folder))
                    return folder;
                string assets = Path.Combine(folder, "assets");
                if (Directory.Exists(assets)) return assets;
                foreach (var sub in Directory.GetDirectories(folder))
                {
                    assets = Path.Combine(sub, "assets");
                    if (Directory.Exists(assets)) return assets;
                }
            }
            catch { }
            return null;
        }

        /// Trophy-Notifier's folder, when it can be found (the Run key, then the usual install folders).
        public static string DetectNotifier()
        {
            var candidates = new List<string>();
            try
            {
                using (var run = Microsoft.Win32.Registry.CurrentUser.OpenSubKey("Software\\Microsoft\\Windows\\CurrentVersion\\Run"))
                    if (run != null)
                        foreach (var name in run.GetValueNames())
                        {
                            string value = run.GetValue(name) as string ?? "";
                            if (value.IndexOf("TrophyNotifier", StringComparison.OrdinalIgnoreCase) < 0 &&
                                name.IndexOf("TrophyNotifier", StringComparison.OrdinalIgnoreCase) < 0)
                                continue;
                            string exe = value.Trim();
                            if (exe.StartsWith("\"")) { int q = exe.IndexOf('"', 1); exe = q > 0 ? exe.Substring(1, q - 1) : exe.Trim('"'); }
                            else { int x = exe.IndexOf(".exe", StringComparison.OrdinalIgnoreCase); if (x > 0) exe = exe.Substring(0, x + 4); }
                            try
                            {
                                string dir = Path.GetDirectoryName(exe);
                                if (!string.IsNullOrEmpty(dir)) { candidates.Add(dir); candidates.Add(Path.GetDirectoryName(dir)); }
                            }
                            catch { }
                        }
            }
            catch { }
            foreach (var bas in new[] { Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                                        Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
                                        Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86) })
            {
                if (string.IsNullOrEmpty(bas)) continue;
                foreach (var name in NotifierNames)
                {
                    candidates.Add(Path.Combine(bas, "Programs", name));
                    candidates.Add(Path.Combine(bas, name));
                }
            }
            foreach (var c in candidates)
                if (AssetsRootOf(c) != null) return c;
            return null;
        }

        static string FindFile(string assets, Func<string, bool> match)
        {
            try
            {
                return Directory.GetFiles(assets, "*", SearchOption.AllDirectories)
                                .OrderBy(f => f.Length).FirstOrDefault(f => match(Path.GetFileName(f).ToLowerInvariant()));
            }
            catch { return null; }
        }

        /// Copies the font, cups and sounds into user\trophy_assets; returns a line per file.
        public static List<string> ImportNotifier(string root, string folder)
        {
            string assets = AssetsRootOf(folder);
            if (assets == null) throw new DirectoryNotFoundException("No assets folder in " + folder + ".");
            string target = AssetsDir(root);
            Directory.CreateDirectory(Path.Combine(target, "fonts"));
            var report = new List<string>();
            Action<string, string> copy = (source, name) =>
            {
                if (source == null) { report.Add(name + ": not found"); return; }
                File.Copy(source, Path.Combine(target, name), true);
                report.Add(name + ": copied");
            };
            string font = Path.Combine(assets, "fonts", "SSTRg.ttf");
            copy(File.Exists(font) ? font : FindFile(assets, n => n == "sstrg.ttf"), Path.Combine("fonts", "SSTRg.ttf"));
            foreach (var cup in new[] { "bronze", "silver", "gold", "platinum" })
            {
                string c = cup;
                copy(FindFile(assets, n => n == c + ".png") ?? FindFile(assets, n => n.EndsWith(".png") && n.Contains(c)), c + ".png");
            }
            foreach (var sound in new[] { "ps5_trophy.wav", "ps5_platinum.wav" })
            {
                string s = sound;
                copy(FindFile(assets, n => n == s), s);
            }
            return report;
        }
    }

    /// What the Trophies page shows, read from the game's TRP with the key.
    class TrophySet
    {
        public string NpCommId, TrpPath, TitleName = "";
        public List<TrophyInfo> Trophies = new List<TrophyInfo>();
        public Dictionary<string, Image> Icons = new Dictionary<string, Image>(StringComparer.OrdinalIgnoreCase);
        public string UnlockFile;

        /// Finds trophy00.trp and decrypts TROPCONF.ESFM with each npCommId candidate. Returns the
        /// working npCommId (or throws with the reason).
        public static string Test(string root, List<string> layers, string hex, out string trpPath, out byte[] trp, out List<TrpEntry> entries)
        {
            trpPath = TrophyStore.FindGameFile(layers, Path.Combine("sce_sys", "trophy", "trophy00.trp"));
            if (trpPath == null) throw new FileNotFoundException("sce_sys\\trophy\\trophy00.trp was not found in ryGT7's app0, the update or the game folder.");
            trp = File.ReadAllBytes(trpPath);
            entries = TrophyCrypto.ParseTrp(trp);
            var conf = TrophyCrypto.Find(entries, "TROPCONF.ESFM");
            if (conf == null) throw new InvalidDataException("trophy00.trp has no TROPCONF.ESFM.");
            var ids = TrophyStore.NpCommIds(root, layers);
            if (ids.Count == 0) throw new InvalidDataException("The game's npCommId was not found (sce_sys\\npbind.dat).");
            byte[] key = TrophyCrypto.HexToBytes(hex);
            var tried = new List<string>();
            foreach (var id in ids)
            {
                string xml;
                try { xml = TrophyCrypto.DecryptEntry(trp, entries, conf.Name, key, id); }
                catch (Exception e) { tried.Add(id + ": " + e.Message); continue; }
                if (TrophyCrypto.LooksLikeTrophyConf(xml)) return id;
                tried.Add(id + ": " + TrophyCrypto.Describe(xml));
            }
            throw new InvalidDataException("The key does not decrypt the game's trophy file. Check the key.\n\nTried " +
                                           string.Join("; ", tried) + " (" + trpPath + ")");
        }

        public static TrophySet Load(string root, List<string> layers, string hex)
        {
            string trpPath; byte[] trp; List<TrpEntry> entries;
            string id = Test(root, layers, hex, out trpPath, out trp, out entries);
            byte[] key = TrophyCrypto.HexToBytes(hex);
            var set = new TrophySet { NpCommId = id, TrpPath = trpPath };
            var trop = TrophyCrypto.Find(entries, "TROP.ESFM") ??
                       entries.Where(e => e.Name.StartsWith("TROP_", StringComparison.OrdinalIgnoreCase) && e.Name.EndsWith(".ESFM", StringComparison.OrdinalIgnoreCase))
                              .OrderBy(e => e.Name).FirstOrDefault();
            if (trop == null) throw new InvalidDataException("trophy00.trp has no TROP.ESFM.");
            string xml = TrophyCrypto.DecryptEntry(trp, entries, trop.Name, key, id);
            set.Trophies = TrophyCrypto.ParseTrophies(xml);
            var title = Regex.Match(xml, "<title-name>([^<]*)</title-name>");
            if (title.Success) set.TitleName = System.Net.WebUtility.HtmlDecode(title.Groups[1].Value.Trim());
            foreach (var entry in entries.Where(e => e.Name.EndsWith(".PNG", StringComparison.OrdinalIgnoreCase)))
            {
                try
                {
                    // Icons stay in memory only.
                    using (var stream = new MemoryStream(TrophyCrypto.EntryBytes(trp, entry)))
                    using (var image = Image.FromStream(stream))
                        set.Icons[entry.Name] = new Bitmap(image, new Size(56, 56));
                }
                catch { }
            }
            var unlocks = TrophyStore.UnlockFiles(root, id);
            if (unlocks.Count > 0)
            {
                set.UnlockFile = unlocks[0];
                try
                {
                    var earned = TrophyCrypto.ParseUnlocks(File.ReadAllText(unlocks[0]));
                    foreach (var t in set.Trophies)
                    {
                        DateTime? when;
                        if (earned.TryGetValue(t.Id, out when)) { t.Earned = true; t.Unlocked = when; }
                    }
                }
                catch { }
            }
            return set;
        }

        public Image IconFor(TrophyInfo t)
        {
            Image image;
            return Icons.TryGetValue("TROP" + t.Id.ToString("000", CultureInfo.InvariantCulture) + ".PNG", out image) ? image : null;
        }
    }

    partial class LoaderForm
    {
        TableLayoutPanel trophyList;
        Label trophySummary;
        TextBox trophyKeyBox;
        bool trophiesLoaded;

        List<string> TrophyLayers() { return TrophyStore.App0Layers(root, settings.Update, settings.Game); }

        /// The key in the box when it is a valid key, else the stored one (null: none).
        string TrophyKey()
        {
            string typed = TrophyCrypto.CleanKey(trophyKeyBox.Text);
            if (TrophyCrypto.ValidKey(typed)) return typed;
            return TrophyStore.LoadKey(root);
        }

        void BuildTrophiesPage()
        {
            TrophyStore.CleanUpLeftoverKeyFile(root);
            var page = NewPage("Trophies", "Trophies",
                "Gran Turismo 7's trophies, read from your own game files. They need your trophy key (32 hexadecimal characters), " +
                "which stays on this PC, encrypted for your Windows account.");

            // ---- the key
            var keySection = Section(page, "Trophy key");
            keySection.Controls.Add(FieldLabel("Key"));
            var field = new Card(Dark.Field) { AutoSize = false, Size = new Size(420, 32), Radius = 3, Edge = Color.Empty, Padding = new Padding(10, 7, 10, 6), Margin = new Padding(0, 0, 0, 2) };
            trophyKeyBox = new TextBox { Dock = DockStyle.Fill, BorderStyle = BorderStyle.None, Margin = new Padding(0), MaxLength = 64, UseSystemPasswordChar = true };
            trophyKeyBox.Text = TrophyStore.LoadKey(root) ?? "";
            field.Controls.Add(trophyKeyBox);
            keySection.Controls.Add(field);
            var keyState = Hint(TrophyStore.HasKey(root) ? "A key is saved." : "No key saved: trophies stay off in the game.");
            keySection.Controls.Add(keyState);
            var show = new Button { Text = "Show", AutoSize = true, Margin = new Padding(0, 0, 10, 0) };
            show.Click += delegate
            {
                trophyKeyBox.UseSystemPasswordChar = !trophyKeyBox.UseSystemPasswordChar;
                show.Text = trophyKeyBox.UseSystemPasswordChar ? "Show" : "Hide";
            };
            var save = new Button { Text = "Save", AutoSize = true, Tag = "accent", Margin = new Padding(0, 0, 10, 0) };
            save.Click += delegate
            {
                string hex = TrophyCrypto.CleanKey(trophyKeyBox.Text);
                if (!TrophyCrypto.ValidKey(hex))
                {
                    MessageBox.Show(this, "The trophy key is 32 hexadecimal characters (0-9, A-F).", "Trophies");
                    return;
                }
                try
                {
                    TrophyStore.SaveKey(root, hex);
                    keyState.Text = "A key is saved.";
                    trophiesLoaded = false;
                    RefreshTrophies();
                }
                catch (Exception e) { MessageBox.Show(this, "Could not save the key: " + e.Message, "Trophies"); }
            };
            var test = new Button { Text = "Test", AutoSize = true, Margin = new Padding(0, 0, 10, 0) };
            test.Click += delegate { TestTrophyKey(); };
            var remove = new Button { Text = "Remove", AutoSize = true, Margin = new Padding(0) };
            remove.Click += delegate
            {
                if (MessageBox.Show(this, "Remove the saved trophy key? The game then runs without trophies.", "Trophies", MessageBoxButtons.OKCancel) != DialogResult.OK)
                    return;
                try { TrophyStore.DeleteKey(root); } catch (Exception e) { MessageBox.Show(this, "Could not remove the key: " + e.Message, "Trophies"); return; }
                trophyKeyBox.Text = "";
                keyState.Text = "No key saved: trophies stay off in the game.";
                trophiesLoaded = false;
                RefreshTrophies();
            };
            var keyButtons = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0, 10, 0, 0), BackColor = Color.Transparent };
            keyButtons.Controls.AddRange(new Control[] { show, save, test, remove });
            keySection.Controls.Add(keyButtons);

            // ---- pop-ups
            var popups = Section(page, "Pop-ups");
            var toast = new ToggleRow { Text = "Show trophy pop-ups", Checked = TrophyStore.ToastsOn(root) };
            toast.CheckedChanged += delegate
            {
                try { TrophyStore.SetToasts(root, toast.Checked); }
                catch (Exception e) { MessageBox.Show(this, "Could not save the setting: " + e.Message, "Trophies"); }
            };
            popups.Controls.Add(toast);
            popups.Controls.Add(OpenRow("Pop-up font, cups and sounds from Trophy-Notifier", ImportTrophyNotifier, "Import from Trophy-Notifier"));
            popups.Controls.Add(Hint("Copies Trophy-Notifier's font, cup pictures and PS5 sounds into user\\trophy_assets. Changes apply the next time you play."));

            // ---- the list
            var listSection = Section(page, "Trophies");
            var head = new TableLayoutPanel { ColumnCount = 2, AutoSize = true, Dock = DockStyle.Fill, Margin = new Padding(0, 6, 0, 6), BackColor = Color.Transparent };
            head.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            head.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            trophySummary = new Label { Text = "", AutoSize = true, Anchor = AnchorStyles.Left, Font = new Font("Segoe UI Semibold", 11f), Margin = new Padding(0, 6, 0, 0) };
            var refresh = new Button { Text = "Refresh", AutoSize = true, Margin = new Padding(0) };
            refresh.Click += delegate { trophiesLoaded = false; RefreshTrophies(); };
            head.Controls.Add(trophySummary, 0, 0);
            head.Controls.Add(refresh, 1, 0);
            listSection.Controls.Add(head);
            trophyList = new TableLayoutPanel { ColumnCount = 1, AutoSize = true, Dock = DockStyle.Fill, Margin = new Padding(0), BackColor = Color.Transparent };
            trophyList.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            listSection.Controls.Add(trophyList);

            // The list is read the first time the page shows.
            pages["Trophies"].VisibleChanged += delegate { if (pages["Trophies"].Visible && !trophiesLoaded) RefreshTrophies(); };
        }

        void TestTrophyKey()
        {
            string hex = TrophyKey();
            if (hex == null)
            {
                MessageBox.Show(this, "Enter your 32-character trophy key first.", "Trophies");
                return;
            }
            try
            {
                Cursor = Cursors.WaitCursor;
                string trpPath; byte[] trp; List<TrpEntry> entries;
                string id = TrophySet.Test(root, TrophyLayers(), hex, out trpPath, out trp, out entries);
                Cursor = Cursors.Default;
                MessageBox.Show(this, "OK: the key decrypts the game's trophies (" + id + ").\n" + trpPath, "Trophies");
            }
            catch (Exception e)
            {
                Cursor = Cursors.Default;
                MessageBox.Show(this, "The test failed: " + e.Message, "Trophies");
            }
        }

        void ImportTrophyNotifier()
        {
            string detected = TrophyStore.DetectNotifier();
            using (var dialog = new FolderBrowserDialog { Description = "Trophy-Notifier's folder (the one with its assets folder)", ShowNewFolderButton = false })
            {
                if (detected != null) dialog.SelectedPath = detected;
                if (dialog.ShowDialog(this) != DialogResult.OK) return;
                try
                {
                    var report = TrophyStore.ImportNotifier(root, dialog.SelectedPath);
                    MessageBox.Show(this, "Imported into user\\trophy_assets:\n" + string.Join("\n", report), "Trophies");
                }
                catch (Exception e) { MessageBox.Show(this, "Could not import: " + e.Message, "Trophies"); }
            }
        }

        static readonly Dictionary<char, Color> GradeColors = new Dictionary<char, Color>
        {
            { 'B', Color.FromArgb(205, 127, 50) }, { 'S', Color.FromArgb(192, 196, 204) },
            { 'G', Color.FromArgb(230, 190, 60) }, { 'P', Color.FromArgb(150, 200, 235) }
        };
        static readonly Dictionary<char, string> GradeNames = new Dictionary<char, string>
        {
            { 'B', "Bronze" }, { 'S', "Silver" }, { 'G', "Gold" }, { 'P', "Platinum" }
        };

        static Image Greyed(Image image)
        {
            if (image == null) return null;
            var grey = new Bitmap(image.Width, image.Height);
            using (var g = Graphics.FromImage(grey))
            using (var attributes = new ImageAttributes())
            {
                attributes.SetColorMatrix(new ColorMatrix(new[]
                {
                    new[] { .3f, .3f, .3f, 0, 0 }, new[] { .59f, .59f, .59f, 0, 0 }, new[] { .11f, .11f, .11f, 0, 0 },
                    new[] { 0f, 0, 0, .55f, 0 }, new[] { 0f, 0, 0, 0, 1 }
                }));
                g.DrawImage(image, new Rectangle(0, 0, image.Width, image.Height), 0, 0, image.Width, image.Height, GraphicsUnit.Pixel, attributes);
            }
            return grey;
        }

        void RefreshTrophies()
        {
            if (trophyList == null) return;
            trophiesLoaded = true;
            trophyList.SuspendLayout();
            foreach (Control old in trophyList.Controls.Cast<Control>().ToList())
            {
                trophyList.Controls.Remove(old);
                old.Dispose();
            }
            string hex = TrophyStore.LoadKey(root);
            if (hex == null)
            {
                trophySummary.Text = "No trophy key saved";
                trophyList.ResumeLayout();
                return;
            }
            TrophySet set;
            try
            {
                Cursor = Cursors.WaitCursor;
                set = TrophySet.Load(root, TrophyLayers(), hex);
            }
            catch (Exception e)
            {
                Cursor = Cursors.Default;
                trophySummary.Text = "Trophies could not be read";
                trophyList.Controls.Add(Hint(e.Message));
                trophyList.ResumeLayout();
                return;
            }
            Cursor = Cursors.Default;
            int earned = set.Trophies.Count(t => t.Earned), total = set.Trophies.Count;
            trophySummary.Text = earned + " of " + total + " earned (" + (total == 0 ? 0 : (int)Math.Round(100.0 * earned / total)) + "%)";
            if (set.UnlockFile == null)
                trophyList.Controls.Add(Hint("Nothing earned yet (no unlock file for " + set.NpCommId + " in user\\home\\1000\\trophy)."));
            foreach (var t in set.Trophies.OrderBy(t => t.Id))
                trophyList.Controls.Add(TrophyRow(set, t));
            trophyList.ResumeLayout();
        }

        Control TrophyRow(TrophySet set, TrophyInfo t)
        {
            bool secret = t.Hidden && !t.Earned;
            var row = new TableLayoutPanel { ColumnCount = 3, RowCount = 1, AutoSize = true, Dock = DockStyle.Fill, Margin = new Padding(0, 0, 0, 1), BackColor = Color.Transparent, Padding = new Padding(0, 6, 0, 6) };
            row.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 72));
            row.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            row.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            row.Paint += (sender, e) => { using (var line = new Pen(Dark.Line)) e.Graphics.DrawLine(line, 0, row.Height - 1, row.Width, row.Height - 1); };
            Image icon = secret ? null : set.IconFor(t);
            if (icon != null && !t.Earned) icon = Greyed(icon);
            var picture = new PictureBox { Size = new Size(56, 56), SizeMode = PictureBoxSizeMode.Zoom, Image = icon, BackColor = icon == null ? Dark.Card : Color.Transparent, Margin = new Padding(0, 0, 16, 0) };
            var text = new TableLayoutPanel { ColumnCount = 1, AutoSize = true, Dock = DockStyle.Fill, Margin = new Padding(0), BackColor = Color.Transparent };
            text.Controls.Add(new Label
            {
                Text = secret ? "Hidden trophy" : t.Name, AutoSize = true, MaximumSize = new Size(640, 0),
                Font = new Font("Segoe UI Semibold", 10f), ForeColor = t.Earned ? Dark.Text : Dark.DimText, Tag = "heading", Margin = new Padding(0, 4, 0, 2)
            });
            text.Controls.Add(new Label
            {
                Text = secret ? "Keep playing to reveal this trophy." : t.Detail, AutoSize = true, MaximumSize = new Size(640, 0),
                ForeColor = Dark.DimText, Tag = "heading", Margin = new Padding(0)
            });
            string when = t.Earned ? (t.Unlocked.HasValue ? "Earned " + t.Unlocked.Value.ToString("g", CultureInfo.CurrentCulture) : "Earned") : "Not earned";
            var state = new Label
            {
                Text = GradeNames[t.Grade] + "\n" + when, AutoSize = true, TextAlign = ContentAlignment.MiddleRight, Anchor = AnchorStyles.Right,
                ForeColor = t.Earned ? GradeColors[t.Grade] : Dark.DimText, Tag = "heading", Margin = new Padding(12, 8, 0, 0)
            };
            row.Controls.Add(picture, 0, 0);
            row.Controls.Add(text, 1, 0);
            row.Controls.Add(state, 2, 0);
            return row;
        }
    }
}
