// SPDX-License-Identifier: GPL-2.0-only
// FEAT-GT7-187: the launcher's Trophies page. The player's own 32-hex trophy key (kept with
// Windows DPAPI in user\trophy.key.dpapi, never in plain text and never logged), a Test of it
// against the game's sce_sys\trophy\trophy00.trp, the trophy list with what is earned (read from
// the game's per-user unlock file), the pop-up switch and the custom pop-up files (FEAT-GT7-189).
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
            if (string.IsNullOrEmpty(text)) return "unreadable bytes"; // DecryptEsfm found no '>'
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

        /// FEAT-GT7-189: "show a test pop-up next time I play" (testtoast=B/S/G/P in user\trophy.ini).
        public static char TestToastPending(string root)
        {
            try
            {
                if (File.Exists(IniPath(root)))
                    foreach (var line in File.ReadAllLines(IniPath(root)))
                    {
                        string l = line.Trim().Replace(" ", "");
                        if (l.StartsWith("testtoast=", StringComparison.OrdinalIgnoreCase) && l.Length > 10 && "BSGP".IndexOf(char.ToUpperInvariant(l[10])) >= 0)
                            return char.ToUpperInvariant(l[10]);
                    }
            }
            catch { }
            return '\0';
        }

        public static void SetTestToast(string root, char grade)
        {
            Directory.CreateDirectory(UserDir(root));
            var lines = File.Exists(IniPath(root)) ? File.ReadAllLines(IniPath(root)).ToList() : new List<string>();
            lines.RemoveAll(l => l.TrimStart().StartsWith("testtoast=", StringComparison.OrdinalIgnoreCase));
            if (grade != '\0') lines.Add("testtoast=" + grade);
            File.WriteAllLines(IniPath(root), lines);
        }

        /// The pending test grade, cleared so it shows once.
        public static char TakeTestToast(string root)
        {
            char grade = TestToastPending(root);
            if (grade != '\0') { try { SetTestToast(root, '\0'); } catch { } }
            return grade;
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
            // FEAT-GT7-189: the pop-up's built-in files, and a one-time test pop-up when asked for.
            try { WriteBuiltinAssets(root); } catch { }
            char test = TakeTestToast(root);
            if (test != '\0') start.EnvironmentVariables["RYGT_TROPHY_TOAST_TEST"] = test.ToString();
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

        /// FEAT-GT7-189: where the game keeps trophy progress, next to the saves.
        public static string ProgressDir(string root) { return Path.Combine(root, "_sd", "trophies_progress"); }

        /// The unlock files, best first (npCommId null: all): _sd\trophies_progress (the first player,
        /// then its 1000+slot folders), then the older user\home\*\trophy files not moved yet.
        public static List<string> UnlockFiles(string root, string npCommId)
        {
            var dirs = new List<string>();
            string progress = ProgressDir(root);
            dirs.Add(progress);
            try { if (Directory.Exists(progress)) dirs.AddRange(Directory.GetDirectories(progress).OrderBy(d => d)); }
            catch { }
            string home = Path.Combine(UserDir(root), "home");
            string first = Path.Combine(home, "1000");
            dirs.Add(Path.Combine(first, "trophy"));
            try
            {
                if (Directory.Exists(home))
                    dirs.AddRange(Directory.GetDirectories(home).Where(d => !d.Equals(first, StringComparison.OrdinalIgnoreCase))
                                           .OrderBy(d => d).Select(d => Path.Combine(d, "trophy")));
            }
            catch { }
            var list = new List<string>();
            foreach (var dir in dirs)
            {
                try
                {
                    if (!Directory.Exists(dir)) continue;
                    if (npCommId != null)
                    {
                        string file = Path.Combine(dir, npCommId + ".xml");
                        if (File.Exists(file)) list.Add(file);
                    }
                    else list.AddRange(Directory.GetFiles(dir, "NPWR*.xml"));
                }
                catch { }
            }
            return list;
        }

        // ---- the pop-up's pictures, sounds and font (FEAT-GT7-189)
        /// The built-in set shipped inside RyGT7.exe (original art, chimes and the Inter font).
        public static readonly string[] BuiltinFiles = { "bronze.png", "silver.png", "gold.png", "platinum.png", "trophy.wav", "platinum.wav", "font.ttf", "font-LICENSE.txt" };
        public static string BuiltinDir(string root) { return Path.Combine(UserDir(root), "trophy_builtin"); }
        public const string ReadmeName = "README.txt";

        static Stream BuiltinStream(string name)
        {
            return typeof(TrophyStore).Assembly.GetManifestResourceStream("trophy." + name);
        }

        /// Writes the built-in set to user\trophy_builtin (the game reads it there when the custom
        /// folder lacks a file). Files already there with the same size are kept. Returns how many
        /// built-in files exist afterwards.
        public static int WriteBuiltinAssets(string root)
        {
            string dir = BuiltinDir(root);
            int present = 0;
            foreach (var name in BuiltinFiles)
            {
                try
                {
                    using (var stream = BuiltinStream(name))
                    {
                        if (stream == null) continue;
                        Directory.CreateDirectory(dir);
                        string path = Path.Combine(dir, name);
                        if (!File.Exists(path) || new FileInfo(path).Length != stream.Length)
                        {
                            string temp = path + ".new";
                            using (var file = File.Create(temp)) stream.CopyTo(file);
                            if (File.Exists(path)) File.Delete(path);
                            File.Move(temp, path);
                        }
                        present++;
                    }
                }
                catch { }
            }
            return present;
        }

        /// The file the game will use for one asset: the custom folder first (any of `names`), then
        /// the built-in set. Null when neither has it.
        public static string AssetFile(string root, params string[] names)
        {
            foreach (var dir in new[] { AssetsDir(root), BuiltinDir(root) })
                foreach (var name in names)
                {
                    string path = Path.Combine(dir, name);
                    if (File.Exists(path)) return path;
                }
            return null;
        }
        public static string CupFile(string root, char grade)
        {
            string name = grade == 'P' ? "platinum" : grade == 'G' ? "gold" : grade == 'S' ? "silver" : "bronze";
            return AssetFile(root, name + ".png");
        }
        public static string SoundFile(string root, bool platinum)
        {
            return platinum ? AssetFile(root, "platinum.wav", "ps5_platinum.wav") : AssetFile(root, "trophy.wav", "ps5_trophy.wav");
        }
        public static string FontFile(string root) { return AssetFile(root, "font.ttf", "font.otf", Path.Combine("fonts", "SSTRg.ttf")); }

        public const string ReadmeText =
"ryGT7 custom trophy pop-up files\r\n" +
"================================\r\n" +
"\r\n" +
"Put your own pictures, sounds or font in this folder to change the in-game trophy pop-up.\r\n" +
"Any file you leave out uses ryGT7's built-in one. Changes apply the next time you play.\r\n" +
"\r\n" +
"Cup pictures (the small cup next to the trophy name)\r\n" +
"  bronze.png  silver.png  gold.png  platinum.png\r\n" +
"  PNG with a transparent background, square (same width and height).\r\n" +
"  128 x 128 or larger; 256 x 256 is recommended. They are drawn at about 30 px at 1080p.\r\n" +
"\r\n" +
"Sounds\r\n" +
"  trophy.wav    plays for Bronze, Silver and Gold\r\n" +
"  platinum.wav  plays for the Platinum trophy\r\n" +
"  WAV, uncompressed PCM, 16-bit, 44.1 kHz or 48 kHz, mono or stereo. Keep them short (under 5 s).\r\n" +
"  MP3 or OGG files are not played.\r\n" +
"\r\n" +
"Font (the trophy name and the \"You earned a trophy!\" line)\r\n" +
"  font.ttf  or  font.otf\r\n" +
"  TrueType or OpenType. Names in Chinese, Japanese or Korean always use a built-in font\r\n" +
"  that carries those characters.\r\n" +
"\r\n" +
"Names are not case sensitive. Older names still work: ps5_trophy.wav, ps5_platinum.wav and\r\n" +
"fonts\\SSTRg.ttf.\r\n" +
"\r\n" +
"The trophy's own picture (the large square on the left) comes from the game and cannot be replaced.\r\n" +
"Only use files you have the right to use.\r\n";

        /// Creates user\trophy_assets with its README.txt (rewritten when ryGT7's text changed).
        public static string EnsureCustomFolder(string root)
        {
            string dir = AssetsDir(root);
            Directory.CreateDirectory(dir);
            string readme = Path.Combine(dir, ReadmeName);
            try
            {
                if (!File.Exists(readme) || File.ReadAllText(readme) != ReadmeText)
                    File.WriteAllText(readme, ReadmeText, Encoding.UTF8);
            }
            catch { }
            return dir;
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
            // FEAT-GT7-189: the 56 px icons are kept in user\trophy\<npCommId>\launcher_icons after the
            // first read, so later opens load small files instead of decoding every TRP picture.
            string cache = Path.Combine(TrophyStore.UserDir(root), "trophy", id, "launcher_icons");
            foreach (var entry in entries.Where(e => e.Name.EndsWith(".PNG", StringComparison.OrdinalIgnoreCase)))
            {
                string cached = Path.Combine(cache, entry.Name);
                try
                {
                    if (File.Exists(cached))
                    {
                        set.Icons[entry.Name] = LoadBitmap(File.ReadAllBytes(cached));
                        continue;
                    }
                }
                catch { }
                try
                {
                    using (var stream = new MemoryStream(TrophyCrypto.EntryBytes(trp, entry)))
                    using (var image = Image.FromStream(stream))
                    {
                        var small = new Bitmap(56, 56, PixelFormat.Format32bppArgb);
                        using (var g = Graphics.FromImage(small))
                        {
                            g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
                            g.DrawImage(image, new Rectangle(0, 0, 56, 56));
                        }
                        set.Icons[entry.Name] = small;
                        try
                        {
                            Directory.CreateDirectory(cache);
                            small.Save(cached, ImageFormat.Png);
                        }
                        catch { }
                    }
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

        /// A bitmap that does not keep its source stream or file open.
        static Bitmap LoadBitmap(byte[] bytes)
        {
            using (var stream = new MemoryStream(bytes))
            using (var image = Image.FromStream(stream))
                return new Bitmap(image);
        }

        /// The list's picture per trophy id: grey when not earned, none for a hidden one not earned.
        public Dictionary<int, Image> RowIcons = new Dictionary<int, Image>();

        public void PrepareRowIcons()
        {
            foreach (var t in Trophies)
            {
                if (t.Hidden && !t.Earned) continue;
                Image icon = IconFor(t);
                if (icon == null) continue;
                RowIcons[t.Id] = t.Earned ? icon : Greyed(icon);
            }
        }

        static Image Greyed(Image image)
        {
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
            popups.Controls.Add(OpenRow("What a trophy pop-up looks like and sounds like", delegate { ShowTrophyPreview(null); }, "Preview"));
            popups.Controls.Add(OpenRow("Your own cup pictures, sounds and font", OpenCustomTrophyFolder, "Open custom trophy images and sounds folder"));
            popups.Controls.Add(Hint("ryGT7 has its own cups, sounds and font. Files you put in this folder replace them; its README.txt lists the names, " +
                                     "sizes and formats. Changes apply the next time you play."));

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

            // The list is read the first time the page shows, off the window's thread.
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
            string id;
            try
            {
                Cursor = Cursors.WaitCursor;
                string trpPath; byte[] trp; List<TrpEntry> entries;
                id = TrophySet.Test(root, TrophyLayers(), hex, out trpPath, out trp, out entries);
            }
            catch (Exception e)
            {
                Cursor = Cursors.Default;
                MessageBox.Show(this, "The test failed: " + e.Message, "Trophies");
                return;
            }
            Cursor = Cursors.Default;
            // FEAT-GT7-189: a passing test shows the pop-up as the game will.
            ShowTrophyPreview("Your key works: it decrypts the game's trophies (" + id + ").");
        }

        void ShowTrophyPreview(string header)
        {
            try { TrophyStore.WriteBuiltinAssets(root); } catch { }
            using (var form = new TrophyPreviewForm(root, lastTrophySet, header))
                form.ShowDialog(this);
        }

        void OpenCustomTrophyFolder()
        {
            try
            {
                string dir = TrophyStore.EnsureCustomFolder(root);
                System.Diagnostics.Process.Start("explorer.exe", "\"" + dir + "\"");
            }
            catch (Exception e) { MessageBox.Show(this, "Could not open the folder: " + e.Message, "Trophies"); }
        }

        TrophySet lastTrophySet;
        int trophyGeneration;

        /// FEAT-GT7-189: reading the TRP (decrypting it and decoding its pictures) runs on a pool
        /// thread; the window only adds the finished rows, one light control each.
        void RefreshTrophies()
        {
            if (trophyList == null) return;
            trophiesLoaded = true;
            int generation = ++trophyGeneration;
            ClearTrophyList();
            string hex = TrophyStore.LoadKey(root);
            if (hex == null)
            {
                trophySummary.Text = "No trophy key saved";
                return;
            }
            trophySummary.Text = "Loading trophies\u2026";
            string where = root;
            var layers = TrophyLayers();
            System.Threading.ThreadPool.QueueUserWorkItem(delegate
            {
                TrophySet set = null;
                Exception error = null;
                try
                {
                    set = TrophySet.Load(where, layers, hex);
                    set.PrepareRowIcons();
                }
                catch (Exception e) { error = e; }
                try
                {
                    if (!IsDisposed && IsHandleCreated)
                        BeginInvoke((Action)delegate { ShowTrophies(generation, set, error); });
                }
                catch { }
            });
        }

        void ClearTrophyList()
        {
            trophyList.SuspendLayout();
            foreach (Control old in trophyList.Controls.Cast<Control>().ToList())
            {
                trophyList.Controls.Remove(old);
                old.Dispose();
            }
            trophyList.ResumeLayout();
        }

        void ShowTrophies(int generation, TrophySet set, Exception error)
        {
            if (generation != trophyGeneration || trophyList == null || trophyList.IsDisposed) return;
            ClearTrophyList();
            trophyList.SuspendLayout();
            if (error != null)
            {
                trophySummary.Text = "Trophies could not be read";
                trophyList.Controls.Add(Hint(error.Message));
                trophyList.ResumeLayout();
                return;
            }
            lastTrophySet = set;
            int earned = set.Trophies.Count(t => t.Earned), total = set.Trophies.Count;
            trophySummary.Text = earned + " of " + total + " earned (" + (total == 0 ? 0 : (int)Math.Round(100.0 * earned / total)) + "%)";
            if (set.UnlockFile == null)
                trophyList.Controls.Add(Hint("Nothing earned yet (no progress file for " + set.NpCommId + " in _sd\\trophies_progress)."));
            foreach (var t in set.Trophies.OrderBy(t => t.Id))
            {
                Image icon;
                set.RowIcons.TryGetValue(t.Id, out icon);
                trophyList.Controls.Add(new TrophyRowView(t, icon));
            }
            trophyList.ResumeLayout();
        }

        internal static readonly Dictionary<char, Color> GradeColors = new Dictionary<char, Color>
        {
            { 'B', Color.FromArgb(205, 127, 50) }, { 'S', Color.FromArgb(192, 196, 204) },
            { 'G', Color.FromArgb(230, 190, 60) }, { 'P', Color.FromArgb(150, 200, 235) }
        };
        internal static readonly Dictionary<char, string> GradeNames = new Dictionary<char, string>
        {
            { 'B', "Bronze" }, { 'S', "Silver" }, { 'G', "Gold" }, { 'P', "Platinum" }
        };
    }

    /// One trophy in the list: a single painted control (picture, name, description, grade and date).
    class TrophyRowView : Control
    {
        static readonly Font NameFont = new Font("Segoe UI Semibold", 10f);
        static readonly Font DetailFont = new Font("Segoe UI", 9f);
        readonly TrophyInfo trophy;
        readonly Image icon;

        public TrophyRowView(TrophyInfo trophy, Image icon)
        {
            this.trophy = trophy;
            this.icon = icon;
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint |
                     ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Color.Transparent;
            Height = 76;
            Dock = DockStyle.Fill;
            Margin = new Padding(0);
            Tag = "heading";
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            bool secret = trophy.Hidden && !trophy.Earned;
            var picture = new Rectangle(0, (Height - 56) / 2, 56, 56);
            if (icon != null) g.DrawImage(icon, picture);
            else using (var fill = new SolidBrush(Dark.Card)) g.FillRectangle(fill, picture);

            string when = trophy.Earned ? (trophy.Unlocked.HasValue ? "Earned " + trophy.Unlocked.Value.ToString("g", CultureInfo.CurrentCulture) : "Earned") : "Not earned";
            string grade = LoaderForm.GradeNames[trophy.Grade];
            Color stateColor = trophy.Earned ? LoaderForm.GradeColors[trophy.Grade] : Dark.DimText;
            var flags = TextFormatFlags.Right | TextFormatFlags.NoPrefix | TextFormatFlags.SingleLine;
            int stateWidth = Math.Max(TextRenderer.MeasureText(g, grade, DetailFont).Width, TextRenderer.MeasureText(g, when, DetailFont).Width);
            int right = Width - 2;
            TextRenderer.DrawText(g, grade, DetailFont, new Rectangle(right - stateWidth, 18, stateWidth, 18), stateColor, flags);
            TextRenderer.DrawText(g, when, DetailFont, new Rectangle(right - stateWidth, 38, stateWidth, 18), stateColor, flags);

            int x = 72, width = Math.Max(40, right - stateWidth - 16 - x);
            TextRenderer.DrawText(g, secret ? "Hidden trophy" : trophy.Name, NameFont, new Rectangle(x, 10, width, 22),
                                  trophy.Earned ? Dark.Text : Dark.DimText, TextFormatFlags.NoPrefix | TextFormatFlags.SingleLine | TextFormatFlags.EndEllipsis);
            TextRenderer.DrawText(g, secret ? "Keep playing to reveal this trophy." : trophy.Detail, DetailFont, new Rectangle(x, 32, width, 38),
                                  Dark.DimText, TextFormatFlags.NoPrefix | TextFormatFlags.WordBreak | TextFormatFlags.EndEllipsis);
            using (var line = new Pen(Dark.Line)) g.DrawLine(line, 0, Height - 1, Width, Height - 1);
        }
    }

    /// FEAT-GT7-189: the in-game trophy pop-up drawn as the game draws it (card 660 x 112 at 1080p,
    /// the same cups, font and sound the next game will use), with a grade picker and the one-time
    /// "show a test pop-up next time I play" option.
    class TrophyPreviewForm : Form
    {
        readonly string root;
        readonly TrophySet set;
        readonly System.Drawing.Text.PrivateFontCollection fonts = new System.Drawing.Text.PrivateFontCollection();
        readonly Dictionary<char, Image> cups = new Dictionary<char, Image>();
        readonly Dictionary<char, Button> gradeButtons = new Dictionary<char, Button>();
        readonly CardView card;
        readonly CheckBox nextPlay;
        Font titleFont, subtitleFont;
        System.Media.SoundPlayer player;
        char grade = 'G';
        bool loading = true;

        public TrophyPreviewForm(string root, TrophySet set, string header)
        {
            this.root = root;
            this.set = set;
            Text = "Trophy pop-up preview";
            Font = new Font("Segoe UI", 9f);
            StartPosition = FormStartPosition.CenterParent;
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            MinimizeBox = false;
            AutoSize = true;
            AutoSizeMode = AutoSizeMode.GrowAndShrink;
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }
            LoadAssets();

            var outer = new TableLayoutPanel { AutoSize = true, ColumnCount = 1, Padding = new Padding(18, 14, 18, 14) };
            Controls.Add(outer);
            if (header != null)
                outer.Controls.Add(new Label { Text = header, AutoSize = true, MaximumSize = new Size(700, 0), Font = new Font("Segoe UI Semibold", 10f), Margin = new Padding(0, 0, 0, 10), UseMnemonic = false });
            var grades = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0, 0, 0, 10) };
            foreach (char g in "BSGP")
            {
                char pick = g;
                var button = new Button { Text = LoaderForm.GradeNames[g], AutoSize = true, Margin = new Padding(0, 0, 8, 0) };
                button.Click += delegate { SelectGrade(pick, true); };
                gradeButtons[g] = button;
                grades.Controls.Add(button);
            }
            var replay = new Button { Text = "Play sound", AutoSize = true, Margin = new Padding(16, 0, 0, 0) };
            replay.Click += delegate { PlaySound(); };
            grades.Controls.Add(replay);
            outer.Controls.Add(grades);

            card = new CardView(this) { Size = new Size(700, 152), Margin = new Padding(0) };
            outer.Controls.Add(card);

            char pending = TrophyStore.TestToastPending(root);
            nextPlay = new CheckBox { Text = "Show this test pop-up the next time I play (once)", AutoSize = true, Checked = pending != '\0', Margin = new Padding(0, 10, 0, 0) };
            nextPlay.CheckedChanged += delegate { SaveNextPlay(); };
            outer.Controls.Add(nextPlay);
            outer.Controls.Add(new Label
            {
                Text = "Uses ryGT7's built-in cups, sounds and font unless your custom folder has its own.", AutoSize = true,
                MaximumSize = new Size(700, 0), ForeColor = Dark.DimText, Tag = "heading", Margin = new Padding(0, 6, 0, 10)
            });
            var close = new Button { Text = "Close", AutoSize = true, DialogResult = DialogResult.OK, Anchor = AnchorStyles.Right, Margin = new Padding(0) };
            outer.Controls.Add(close);
            AcceptButton = close;
            CancelButton = close;

            SelectGrade(pending != '\0' ? pending : 'G', false);
            loading = false;
            Dark.Apply(this);
            HandleCreated += delegate { Dark.TitleBar(this); };
            Shown += delegate { PlaySound(); };
        }

        void LoadAssets()
        {
            foreach (char g in "BSGP")
            {
                string path = TrophyStore.CupFile(root, g);
                if (path == null) continue;
                try
                {
                    using (var stream = new MemoryStream(File.ReadAllBytes(path)))
                    using (var image = Image.FromStream(stream))
                        cups[g] = new Bitmap(image);
                }
                catch { }
            }
            string font = TrophyStore.FontFile(root);
            if (font != null)
            {
                try
                {
                    fonts.AddFontFile(font);
                    var family = fonts.Families.Length > 0 ? fonts.Families[0] : null;
                    if (family != null)
                    {
                        var style = family.IsStyleAvailable(FontStyle.Regular) ? FontStyle.Regular : FontStyle.Bold;
                        titleFont = new Font(family, 30f, style, GraphicsUnit.Pixel);
                        subtitleFont = new Font(family, 24f, style, GraphicsUnit.Pixel);
                    }
                }
                catch { titleFont = null; subtitleFont = null; }
            }
            if (titleFont == null) titleFont = new Font("Segoe UI Semibold", 30f, FontStyle.Regular, GraphicsUnit.Pixel);
            if (subtitleFont == null) subtitleFont = new Font("Segoe UI Semibold", 24f, FontStyle.Regular, GraphicsUnit.Pixel);
        }

        void SelectGrade(char g, bool sound)
        {
            grade = g;
            foreach (var pair in gradeButtons)
            {
                pair.Value.Tag = pair.Key == g ? "accent" : null;
                pair.Value.Invalidate();
            }
            card.Invalidate();
            if (!loading) SaveNextPlay();
            if (sound) PlaySound();
        }

        void SaveNextPlay()
        {
            if (loading) return;
            try { TrophyStore.SetTestToast(root, nextPlay.Checked ? grade : '\0'); }
            catch (Exception e) { MessageBox.Show(this, "Could not save the setting: " + e.Message, "Trophies"); }
        }

        void PlaySound()
        {
            string path = TrophyStore.SoundFile(root, grade == 'P');
            if (path == null) return;
            try
            {
                if (player != null) { player.Stop(); player.Dispose(); }
                player = new System.Media.SoundPlayer(path);
                player.Play();
            }
            catch { }
        }

        /// The trophy the card shows: one of this grade from the game when the list was read.
        TrophyInfo Sample()
        {
            if (set == null) return null;
            return set.Trophies.Where(t => t.Grade == grade).OrderBy(t => t.Hidden ? 1 : 0).ThenBy(t => t.Id).FirstOrDefault();
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                if (player != null) { try { player.Stop(); } catch { } player.Dispose(); }
                foreach (var image in cups.Values) image.Dispose();
                if (titleFont != null) titleFont.Dispose();
                if (subtitleFont != null) subtitleFont.Dispose();
                fonts.Dispose();
            }
            base.Dispose(disposing);
        }

        class CardView : Control
        {
            readonly TrophyPreviewForm form;
            public CardView(TrophyPreviewForm form)
            {
                this.form = form;
                SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
                BackColor = Color.FromArgb(70, 76, 88);
                Tag = "heading";
            }

            static System.Drawing.Drawing2D.GraphicsPath Rounded(RectangleF r, float radius)
            {
                var path = new System.Drawing.Drawing2D.GraphicsPath();
                float d = radius * 2;
                path.AddArc(r.X, r.Y, d, d, 180, 90);
                path.AddArc(r.Right - d, r.Y, d, d, 270, 90);
                path.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
                path.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
                path.CloseFigure();
                return path;
            }

            void DrawCup(Graphics g, RectangleF r)
            {
                Image cup;
                if (form.cups.TryGetValue(form.grade, out cup)) { g.DrawImage(cup, r); return; }
                using (var brush = new SolidBrush(LoaderForm.GradeColors[form.grade])) g.FillEllipse(brush, r);
            }

            protected override void OnPaint(PaintEventArgs e)
            {
                var g = e.Graphics;
                g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
                g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
                g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.AntiAliasGridFit;
                // A backdrop stands in for the game; the card sits 20 px in, as in the top-right corner.
                using (var backdrop = new System.Drawing.Drawing2D.LinearGradientBrush(ClientRectangle, Color.FromArgb(58, 72, 92), Color.FromArgb(24, 26, 32), 20f))
                    g.FillRectangle(backdrop, ClientRectangle);
                float x = 20, y = 20, cw = 660, ch = 112;
                for (int i = 8; i >= 1; --i)
                    using (var shadow = new SolidBrush(Color.FromArgb(10, 0, 0, 0)))
                    using (var path = Rounded(new RectangleF(x - i, y + 6 - i, cw + 2 * i, ch + 2 * i), 16 + i))
                        g.FillPath(shadow, path);
                using (var fill = new SolidBrush(Color.FromArgb(232, 28, 28, 30)))
                using (var path = Rounded(new RectangleF(x, y, cw, ch), 16))
                    g.FillPath(fill, path);

                bool plat = form.grade == 'P';
                var sample = form.Sample();
                Image art = null;
                if (sample != null && form.set != null) art = form.set.IconFor(sample);
                var artRect = new RectangleF(x + 20, y + (ch - 72) / 2, 72, 72);
                using (var path = Rounded(artRect, 6))
                {
                    if (art != null)
                    {
                        var state = g.Save();
                        g.SetClip(path);
                        g.DrawImage(art, artRect);
                        g.Restore(state);
                    }
                    else
                    {
                        using (var fill = new SolidBrush(Color.FromArgb(44, 46, 54))) g.FillPath(fill, path);
                        DrawCup(g, new RectangleF(artRect.X + 8, artRect.Y + 8, 56, 56));
                    }
                }

                float tx = x + 114;
                if (!plat) DrawCup(g, new RectangleF(tx, y + 22, 30, 30));
                float ttx = plat ? tx : tx + 38, right = x + cw - 20;
                string name = sample != null ? sample.Name : "Test trophy";
                var format = new StringFormat(StringFormatFlags.NoWrap) { Trimming = StringTrimming.EllipsisCharacter };
                using (var title = new SolidBrush(Color.FromArgb(245, 245, 247)))
                    g.DrawString(name, form.titleFont, title, new RectangleF(ttx, y + 17, right - ttx, 40), format);
                using (var subtitle = new SolidBrush(Color.FromArgb(178, 178, 184)))
                    g.DrawString(plat ? "You earned the Platinum trophy!" : "You earned a trophy!", form.subtitleFont, subtitle, new RectangleF(tx, y + 59, right - tx, 34), format);
            }
        }
    }
}
