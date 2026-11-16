// Setup regression checks. Built with test-gt7.cmd; uses synthetic dumps beside its folder.
// SPDX-License-Identifier: GPL-2.0-only
using System;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;

namespace RyGT
{
    static class GT7SetupTests
    {
        [System.Runtime.InteropServices.DllImport("kernel32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
        static extern bool CreateHardLink(string link, string existing, IntPtr security);

        static int checks;
        static void Check(bool value, string message)
        { checks++; if (!value) throw new Exception(message); }
        static void Reject(Action action, string message)
        { try { action(); } catch (InvalidDataException) { checks++; return; } throw new Exception(message); }
        static void Put(string root, string name, string value)
        {
            string path = Path.Combine(root, name);
            Directory.CreateDirectory(Path.GetDirectoryName(path)); File.WriteAllText(path, value);
        }
        static void Sfo(string folder, string id, string version)
        {
            Directory.CreateDirectory(Path.Combine(folder, "sce_sys"));
            byte[] keys = Encoding.UTF8.GetBytes("TITLE_ID\0APP_VER\0");
            byte[] a = Encoding.UTF8.GetBytes(id + "\0"), b = Encoding.UTF8.GetBytes(version + "\0");
            using (var stream = File.Create(Path.Combine(folder, "sce_sys", "param.sfo")))
            using (var writer = new BinaryWriter(stream))
            {
                writer.Write(0x46535000); writer.Write(0x101); writer.Write(52); writer.Write(52 + keys.Length); writer.Write(2);
                writer.Write((ushort)0); writer.Write((ushort)0x204); writer.Write(a.Length); writer.Write(a.Length); writer.Write(0);
                writer.Write((ushort)9); writer.Write((ushort)0x204); writer.Write(b.Length); writer.Write(b.Length); writer.Write(a.Length);
                writer.Write(keys); writer.Write(a); writer.Write(b);
            }
        }
        static void Dump(string folder)
        {
            Sfo(folder, GameLayout.TitleId, "01.00");
            foreach (string name in new[] { "eboot.bin", "contents\\gt.idx", "contents\\gt01.vol", "sce_module\\libc.prx", "sce_module\\libSceFios2.prx", "contents\\base-only.dat", "nested\\base.txt" })
                Put(folder, name, "base:" + name);
        }
        static string PatchXml(string title, string group, string lines)
        {
            return "<?xml version=\"1.0\"?><Patch><TitleID><ID>" + title + "</ID></TitleID>" +
                "<Metadata Name=\"" + group + "\" PatchVer=\"1.0\" AppElf=\"eboot.bin\"><PatchList>" + lines + "</PatchList></Metadata>" +
                "<Metadata Name=\"Unlock Vehicle DLCs\" AppElf=\"eboot.bin\"><PatchList><Line Type=\"bytes\" Address=\"0x400010\" Value=\"ee\"/></PatchList></Metadata></Patch>";
        }
        static void Header(byte[] elf, int index, uint type, long offset, long address, long fileSize, long memorySize, long alignment)
        {
            int at = 64 + index * 56;
            BitConverter.GetBytes(type).CopyTo(elf, at);
            foreach (var field in new[] { new[] { 8, offset }, new[] { 16, address }, new[] { 32, fileSize }, new[] { 40, memorySize }, new[] { 48, alignment } })
                BitConverter.GetBytes(field[1]).CopyTo(elf, at + (int)field[0]);
        }
        static long Field(byte[] elf, int index, int field) { return BitConverter.ToInt64(elf, 64 + index * 56 + field); }
        static void OfflineChecks(string root)
        {
            string xml = Path.Combine(root, "offline.xml");
            Put(root, "offline.xml", PatchXml(GameLayout.TitleId, OfflinePatch.GroupName,
                "<Line Type=\"bytes\" Address=\"0x400180\" Value=\"c3c3c3c3\"/><Line Type=\"bytes\" Address=\"0x00410300\" Value=\"0102\"/>"));
            var patch = OfflinePatch.Load(xml);
            Check(patch.Writes.Count == 2 && patch.Writes[0].Key == 0x180 && patch.Writes[1].Key == 0x10300, "Offline mode lines (and only them) read");
            Put(root, "offline.xml", PatchXml("CUSA03220", OfflinePatch.GroupName, "<Line Type=\"bytes\" Address=\"0x400180\" Value=\"c3\"/>"));
            Reject(() => OfflinePatch.Load(xml), "Patch for another game accepted");
            Put(root, "offline.xml", PatchXml(GameLayout.TitleId, "Debug Settings", "<Line Type=\"bytes\" Address=\"0x400180\" Value=\"c3\"/>"));
            Reject(() => OfflinePatch.Load(xml), "File without an Offline mode group accepted");
            Put(root, "offline.xml", PatchXml(GameLayout.TitleId, OfflinePatch.GroupName, "<Line Type=\"utf8\" Address=\"0x400180\" Value=\"c3\"/>"));
            Reject(() => OfflinePatch.Load(xml), "Unsupported line type accepted");
            Put(root, "offline.xml", PatchXml(GameLayout.TitleId, OfflinePatch.GroupName,
                "<Line Type=\"bytes\" Address=\"0x400180\" Value=\"c3c3\"/><Line Type=\"bytes\" Address=\"0x400181\" Value=\"90\"/>"));
            Reject(() => OfflinePatch.Load(xml), "Conflicting lines accepted");
            Reject(() => patch.ExtendLayout(new byte[64]), "Unverified executable extended");

            // Code with padding up to the next segment grows in place; data with uninitialized memory
            // moves to the end of the file with its TLS header.
            var elf = new byte[0x2080];
            new byte[] { 0x7f, (byte)'E', (byte)'L', (byte)'F', 2, 1 }.CopyTo(elf, 0);
            BitConverter.GetBytes(64L).CopyTo(elf, 0x20);
            BitConverter.GetBytes((ushort)56).CopyTo(elf, 0x36);
            BitConverter.GetBytes((ushort)3).CopyTo(elf, 0x38);
            Header(elf, 0, 1, 0x1000, 0, 0x100, 0x100, 0x1000);
            Header(elf, 1, 1, 0x2000, 0x10000, 0x80, 0x200, 0x1000);
            Header(elf, 2, 7, 0x2040, 0x10040, 0x10, 0x20, 0x10);
            for (int i = 0; i < 0x80; i++) elf[0x2000 + i] = (byte)(i + 1);
            var layout = OfflinePatch.Extend(elf, patch.Writes);
            Check(Field(layout, 0, 8) == 0x1000 && Field(layout, 0, 32) == 0x184 && Field(layout, 0, 40) == 0x184, "Code segment extended in place");
            Check(Field(layout, 1, 8) == 0x3000 && Field(layout, 1, 32) == 0x302 && Field(layout, 1, 40) == 0x302 && layout.Length == 0x3302, "Data segment moved and extended");
            Check(Field(layout, 2, 8) == 0x3040 && layout[0x3000] == 1 && layout[0x307f] == 0x80 && layout[0x3080] == 0, "Data and TLS header moved");
            Check(elf.SequenceEqual(OfflinePatch.Extend(elf, new System.Collections.Generic.List<System.Collections.Generic.KeyValuePair<long, byte[]>>())), "Layout changed without patch lines");

            // A converted program: .elf0 holds guest address 0 at RVA 0x10000, .reloc a 64-bit
            // relocation overlapping the patch line and one that does not.
            var image = new byte[0x800];
            BitConverter.GetBytes(0x80).CopyTo(image, 0x3c);
            BitConverter.GetBytes(0x4550).CopyTo(image, 0x80);
            BitConverter.GetBytes((ushort)2).CopyTo(image, 0x86);
            BitConverter.GetBytes((ushort)240).CopyTo(image, 0x94);
            BitConverter.GetBytes((ushort)0x20b).CopyTo(image, 0x98);
            BitConverter.GetBytes(16).CopyTo(image, 0x98 + 108);
            BitConverter.GetBytes(0x20000).CopyTo(image, 0x98 + 112 + 40);
            BitConverter.GetBytes(12).CopyTo(image, 0x98 + 112 + 44);
            int table = 0x98 + 240;
            foreach (var section in new[] { new object[] { ".elf0", 0x10000, 0x200, 0x400 }, new object[] { ".reloc", 0x20000, 0x200, 0x600 } })
            {
                Encoding.ASCII.GetBytes((string)section[0]).CopyTo(image, table);
                BitConverter.GetBytes((int)section[1]).CopyTo(image, table + 12);
                BitConverter.GetBytes((int)section[2]).CopyTo(image, table + 16);
                BitConverter.GetBytes((int)section[3]).CopyTo(image, table + 20);
                table += 40;
            }
            Buffer.BlockCopy(layout, 0x1000, image, 0x400, 0x184);
            BitConverter.GetBytes(0x10000).CopyTo(image, 0x600);
            BitConverter.GetBytes(12).CopyTo(image, 0x604);
            BitConverter.GetBytes((ushort)(0xa000 | 0x17c)).CopyTo(image, 0x608);
            BitConverter.GetBytes((ushort)(0xa000 | 0x170)).CopyTo(image, 0x60a);
            var write = new[] { patch.Writes[0] }.ToList();
            var mismatch = (byte[])image.Clone();
            mismatch[0x400 + 0x181] ^= 0xff;
            Reject(() => OfflinePatch.Overlay(mismatch, layout, write), "Patch applied over unexpected code");
            Check(OfflinePatch.Overlay(image, layout, write) == 1, "Overlapping relocation disabled");
            Check(BitConverter.ToUInt16(image, 0x608) == 0x17c && BitConverter.ToUInt16(image, 0x60a) == (0xa000 | 0x170), "Only the overlapping relocation disabled");
            Check(image.Skip(0x400 + 0x180).Take(4).All(b => b == 0xc3), "Patch bytes written");
        }
        // FEAT-GT7-187: the TRP / ESFM reading of the Trophies page, on a synthetic encrypted trophy file.
        static void Be32(System.Collections.Generic.List<byte> b, long v) { for (int s = 24; s >= 0; s -= 8) b.Add((byte)(v >> s)); }
        static void Be64(System.Collections.Generic.List<byte> b, long v) { Be32(b, v >> 32); Be32(b, v & 0xffffffffL); }

        internal static byte[] SyntheticTrp(params Tuple<string, byte[], uint>[] files)
        {
            var b = new System.Collections.Generic.List<byte>();
            Be32(b, TrophyCrypto.TrpMagic); Be32(b, 3); Be64(b, 0); Be32(b, files.Length); Be32(b, TrophyCrypto.EntrySize); Be32(b, 0);
            while (b.Count < TrophyCrypto.HeaderSize) b.Add(0);
            long offset = TrophyCrypto.HeaderSize + files.Length * TrophyCrypto.EntrySize;
            foreach (var f in files)
            {
                var name = new byte[32];
                Encoding.ASCII.GetBytes(f.Item1).CopyTo(name, 0);
                b.AddRange(name); Be64(b, offset); Be64(b, f.Item2.Length); Be32(b, f.Item3); b.AddRange(new byte[12]);
                offset += f.Item2.Length;
            }
            foreach (var f in files) b.AddRange(f.Item2);
            return b.ToArray();
        }

        // FEAT-GT7-189: the built-in pop-up files, the custom folder and where progress is read.
        internal static void TrophyFileChecks(string root)
        {
            Directory.CreateDirectory(root);
            Check(TrophyStore.WriteBuiltinAssets(root) == TrophyStore.BuiltinFiles.Length, "Every built-in trophy file is embedded in the launcher");
            Check(TrophyStore.CupFile(root, 'G') == Path.Combine(TrophyStore.BuiltinDir(root), "gold.png"), "The built-in gold cup is used without a custom one");
            Check(TrophyStore.FontFile(root) == Path.Combine(TrophyStore.BuiltinDir(root), "font.ttf"), "The built-in font is used without a custom one");
            string custom = TrophyStore.EnsureCustomFolder(root);
            Check(File.Exists(Path.Combine(custom, TrophyStore.ReadmeName)), "The custom folder gets its README.txt");
            File.WriteAllText(Path.Combine(custom, "ps5_platinum.wav"), "x");
            Check(TrophyStore.SoundFile(root, true) == Path.Combine(custom, "ps5_platinum.wav"), "An older custom sound name still replaces the built-in one");
            Check(TrophyStore.SoundFile(root, false) == Path.Combine(TrophyStore.BuiltinDir(root), "trophy.wav"), "A sound the custom folder lacks stays built in");
            Check(TrophyStore.TestToastPending(root) == '\0', "No test pop-up by default");
            TrophyStore.SetTestToast(root, 'P');
            Check(TrophyStore.TakeTestToast(root) == 'P' && TrophyStore.TestToastPending(root) == '\0', "The next-play test pop-up shows once");
            string id = "NPWR12345_00";
            Put(Path.Combine(root, "user", "home", "1000", "trophy"), id + ".xml", "<trophyconf/>");
            Check(TrophyStore.UnlockFiles(root, id).Count == 1, "Progress not moved yet is read from user\\home");
            Put(TrophyStore.ProgressDir(root), id + ".xml", "<trophyconf/>");
            var files = TrophyStore.UnlockFiles(root, id);
            Check(files.Count == 2 && files[0] == Path.Combine(TrophyStore.ProgressDir(root), id + ".xml"), "Progress in _sd\\trophies_progress comes first");
        }

        internal static void TrophyChecks()
        {
            byte[] key = TrophyCrypto.HexToBytes("000102030405060708090a0b0c0d0e0f");
            string id = "NPWR12345_00";
            byte[] trpKey = TrophyCrypto.DeriveTrpKey(key, id);
            // openssl enc -aes-128-cbc -K 000102..0f -iv 0 -nopad of "NPWR12345_00" + 4 zero bytes
            Check(BitConverter.ToString(trpKey).Replace("-", "").ToLowerInvariant() == "5340e96ec12aa58cf6cc17792f17ffad", "Trophy key derivation (AES-128-CBC of the npCommId)");
            byte[] iv = Enumerable.Range(0, 16).Select(i => (byte)(0xA0 + i)).ToArray();
            string conf = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<trophyconf version=\"1.1\"><npcommid>" + id + "</npcommid></trophyconf>";
            string trop = "<trophyconf version=\"1.1\"><title-name>Gran Turismo&#174; 7</title-name>" +
                          "<trophy id=\"000\" hidden=\"no\" ttype=\"P\" pid=\"-1\"><name>All of them</name><detail>Every trophy</detail></trophy>" +
                          "<trophy id=\"001\" hidden=\"yes\" ttype=\"G\" pid=\"000\"><name>Secret</name><detail>Hidden one</detail></trophy>" +
                          "<trophy id=\"002\" hidden=\"no\" ttype=\"B\" pid=\"000\"><name>First</name><detail>Bronze one</detail></trophy></trophyconf>";
            byte[] trp = SyntheticTrp(Tuple.Create("TROPCONF.ESFM", TrophyCrypto.EncryptEsfm(trpKey, iv, conf), 3u),
                                      Tuple.Create("TROP.ESFM", TrophyCrypto.EncryptEsfm(trpKey, iv, trop), 3u),
                                      Tuple.Create("ICON0.PNG", new byte[] { 0x89, 0x50, 0x4E, 0x47 }, 0u));
            var entries = TrophyCrypto.ParseTrp(trp);
            Check(entries.Count == 3 && entries[0].Name == "TROPCONF.ESFM" && entries[0].Flag == 3 && entries[2].Length == 4, "TRP entry table read");
            string decrypted = TrophyCrypto.DecryptEntry(trp, entries, "TROPCONF.ESFM", key, id);
            Check(decrypted == conf && TrophyCrypto.LooksLikeTrophyConf(decrypted), "TROPCONF.ESFM decrypted");
            Check(!TrophyCrypto.LooksLikeTrophyConf(TrophyCrypto.DecryptEntry(trp, entries, "TROPCONF.ESFM", TrophyCrypto.HexToBytes("ffffffffffffffffffffffffffffffff"), id)),
                  "A wrong trophy key fails the test");
            Check(!TrophyCrypto.LooksLikeTrophyConf(TrophyCrypto.DecryptEntry(trp, entries, "TROPCONF.ESFM", key, "NPWR54321_00")), "A wrong npCommId fails the test");
            string signed = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!--Sce-Np-Trophy-Signature: " + new string('A', 300) + "-->\n" +
                            "<trophyconf version=\"1.1\" platform=\"ps4\"><npcommid>" + id + "</npcommid></trophyconf>";
            byte[] signedTrp = SyntheticTrp(Tuple.Create("TROPCONF.ESFM", TrophyCrypto.EncryptEsfm(trpKey, iv, signed), 3u));
            var signedEntries = TrophyCrypto.ParseTrp(signedTrp);
            Check(TrophyCrypto.LooksLikeTrophyConf(TrophyCrypto.DecryptEntry(signedTrp, signedEntries, "TROPCONF.ESFM", key, id)),
                  "TROPCONF with Sony's signature comment accepted (real GT7 layout)");
            Check(TrophyCrypto.Describe(TrophyCrypto.DecryptEntry(signedTrp, signedEntries, "TROPCONF.ESFM", TrophyCrypto.HexToBytes("ffffffffffffffffffffffffffffffff"), id)) == "unreadable bytes",
                  "A wrong key is reported as unreadable bytes");
            var list = TrophyCrypto.ParseTrophies(TrophyCrypto.DecryptEntry(trp, entries, "TROP.ESFM", key, id));
            Check(list.Count == 3 && list[0].Grade == 'P' && list[1].Hidden && list[1].Grade == 'G' && list[2].Name == "First" && list[2].Detail == "Bronze one",
                  "TROP.ESFM trophies parsed");
            var unlocks = TrophyCrypto.ParseUnlocks("<trophyconf><trophy id=\"000\" hidden=\"no\" ttype=\"P\" pid=\"-1\" unlockstate=\"false\" timestamp=\"0\">" +
                                                    "<name>x</name></trophy><trophy id=\"002\" ttype=\"B\" unlockstate=\"true\" timestamp=\"1763150400\"></trophy></trophyconf>");
            Check(unlocks.Count == 1 && unlocks.ContainsKey(2) && unlocks[2].HasValue &&
                  unlocks[2].Value.ToUniversalTime() == new DateTime(2025, 11, 14, 20, 0, 0, DateTimeKind.Utc), "Unlock file read (Unix seconds)");
            Check(TrophyCrypto.Timestamp("1763150400000").Value.ToUniversalTime() == new DateTime(2025, 11, 14, 20, 0, 0, DateTimeKind.Utc), "Unix milliseconds");
            Check(TrophyCrypto.ValidKey("0123456789abcdefABCDEF0123456789") && !TrophyCrypto.ValidKey("0123") && !TrophyCrypto.ValidKey("0123456789abcdefABCDEF012345678g"),
                  "Trophy key format");
            Reject(() => TrophyCrypto.ParseTrp(new byte[TrophyCrypto.HeaderSize]), "A file without the TRP magic accepted");
            byte[] cut = trp.Take(TrophyCrypto.HeaderSize + TrophyCrypto.EntrySize * 3 + 8).ToArray();
            Reject(() => TrophyCrypto.ParseTrp(cut), "A TRP entry outside the file accepted");
        }
        [STAThread]
        static int Main(string[] args)
        {
            // The synthetic dumps go beside the test's launcher folder, never inside it: setup
            // rejects a game or update folder nested in the launcher folder (or the reverse).
            string runtime = AppDomain.CurrentDomain.BaseDirectory;
            string root = Path.Combine(Path.GetFullPath(Path.Combine(runtime, "..")), "rygt7-tests-" + Guid.NewGuid().ToString("N"));
            string baseDir = Path.Combine(root, "base"), update = Path.Combine(root, "update");
            try
            {
                Dump(baseDir);
                var merged = new GameLayout(baseDir, null, runtime);
                Check(merged.Version == "01.00", "Merged version");
                Sfo(update, GameLayout.TitleId, "01.01");
                Put(update, "eboot.bin", "updated executable");
                Put(update, "contents\\gt.idx", "updated index");
                Put(update, "nested\\update.txt", "update-only");
                var separate = new GameLayout(baseDir, update, runtime);
                Check(separate.Version == "01.01", "Update version");
                Check(separate.Get("eboot.bin") == Path.Combine(update, "eboot.bin"), "Updated executable selected");
                Check(separate.Get("contents/gt.idx") == Path.Combine(update, "contents", "gt.idx"), "Updated index selected");
                Check(separate.Get("contents/base-only.dat") != null, "Base contents preserved");
                Check(separate.Get("nested/base.txt") != null && separate.Get("nested/update.txt") != null, "Nested overlay union");
                Check(separate.Get("sce_module/libc.prx") == Path.Combine(baseDir, "sce_module", "libc.prx"), "Module fallback");
                Reject(() => new GameLayout(baseDir, update, baseDir), "Runtime/source overlap allowed");
                Sfo(update, "CUSA03220", "01.69");
                Reject(() => new GameLayout(baseDir, update, runtime), "Wrong game update accepted");
                Sfo(update, GameLayout.TitleId, "00.99");
                Reject(() => new GameLayout(baseDir, update, runtime), "Older update accepted");
                Sfo(update, GameLayout.TitleId, "01.01");
                File.Delete(Path.Combine(update, "eboot.bin"));
                Reject(() => new GameLayout(baseDir, update, runtime), "Incomplete update accepted");
                Put(update, "eboot.bin", "updated executable");
                Put(update, "sce_sys\\param.sfo", "bad metadata");
                Reject(() => new GameLayout(baseDir, update, runtime), "Bad SFO accepted");
                Sfo(update, GameLayout.TitleId, "01.01");
                string converted = Path.Combine(root, "converted");
                Put(converted, "libc.prx", "converted-library");
                // FEAT-GTS-251: what earlier Set ups left: app0 and an app0.previous-* folder full of hard
                // links to the game's files, plus that Set up's converted modules; one real copy.
                string app0 = Path.Combine(runtime, "app0"), older = Path.Combine(runtime, "app0.previous-test-older");
                string withCopy = Path.Combine(runtime, "app0.previous-test-copy");
                foreach (string old in new[] { app0, older, withCopy })
                {
                    if (Directory.Exists(old)) Directory.Delete(old, true);
                    foreach (var pair in separate.Files)
                    {
                        if (pair.Key.Equals("eboot.bin", StringComparison.OrdinalIgnoreCase) ||
                            pair.Key.StartsWith("sce_module\\", StringComparison.OrdinalIgnoreCase)) continue;
                        string link = Path.Combine(old, pair.Key);
                        Directory.CreateDirectory(Path.GetDirectoryName(link));
                        Check(CreateHardLink(link, pair.Value, IntPtr.Zero), "Hard link for the test");
                    }
                    Put(old, "sce_module\\libc.prx", "old-converted-library");
                }
                File.Delete(Path.Combine(withCopy, "contents", "gt.idx"));
                File.Copy(Path.Combine(update, "contents", "gt.idx"), Path.Combine(withCopy, "contents", "gt.idx"));
                using (var form = new LoaderForm())
                {
                    typeof(LoaderForm).GetMethod("UseGameFoldersDirectly", BindingFlags.NonPublic | BindingFlags.Instance)
                        .Invoke(form, new object[] { separate, converted });
                }
                Check(File.ReadAllText(Path.Combine(app0, "sce_module", "libc.prx")) == "converted-library", "Converted module retained");
                Check(Directory.EnumerateFileSystemEntries(app0).Select(Path.GetFileName).SequenceEqual(new[] { "sce_module" }),
                      "app0 holds only the converted modules");
                Check(!Directory.Exists(older), "A previous app0 of links and modules removed");
                Check(Directory.GetDirectories(runtime, "app0.previous-*").Length == 1 && Directory.Exists(withCopy),
                      "Only the previous app0 holding a real copy kept");
                Check(File.ReadAllText(Path.Combine(withCopy, "contents", "gt.idx")) == "updated index", "Real copy kept");
                Check(!File.Exists(Path.Combine(withCopy, "contents", "base-only.dat")) && !Directory.Exists(Path.Combine(withCopy, "nested")),
                      "Links removed beside the copy");
                foreach (var pair in separate.Files)
                    Check(File.Exists(pair.Value), "Game file " + pair.Key + " survives");
                Check(File.ReadAllText(Path.Combine(update, "contents", "gt.idx")) == "updated index", "Update modified");
                Check(File.ReadAllText(Path.Combine(baseDir, "contents", "gt.idx")) == "base:contents\\gt.idx", "Source modified");
                Directory.Delete(withCopy, true);
                // A runtime that can't read the game folders still gets them linked into app0.
                using (var form = new LoaderForm())
                {
                    typeof(LoaderForm).GetMethod("LinkGameFiles", BindingFlags.NonPublic | BindingFlags.Instance)
                        .Invoke(form, new object[] { separate, converted });
                }
                Check(File.ReadAllText(Path.Combine(app0, "contents", "gt.idx")) == "updated index", "Linked update");
                Check(File.ReadAllText(Path.Combine(app0, "nested", "base.txt")) == "base:nested\\base.txt", "Linked base fallback");
                Check(File.ReadAllText(Path.Combine(app0, "sce_module", "libc.prx")) == "converted-library", "Converted module retained after linking");
                foreach (string old in Directory.GetDirectories(runtime, "app0.previous-*")) Directory.Delete(old, true);
                File.Delete(Path.Combine(baseDir, "contents", "gt01.vol"));
                Reject(() => new GameLayout(baseDir, null, runtime), "Missing base volumes accepted");
                Check(!GameLayout.PatchKnownExecutable(new byte[64]), "Unknown executable patched");
                OfflineChecks(root);
                if (args.Length == 2)
                {
                    byte[] elf = File.ReadAllBytes(args[0]);
                    Check(GameLayout.PatchKnownExecutable(elf), "Known GT7 ELF not recognized");
                    Check(elf.SequenceEqual(File.ReadAllBytes(args[1])), "Conversion fix differs from verified ELF");
                }
                // --offline <patch XML> <ELF after the conversion fix> <expected extended ELF>
                //           <GT7.exe converted from it> <expected patched GT7.exe>
                if (args.Length == 6 && args[0] == "--offline")
                {
                    var patch = OfflinePatch.Load(args[1]);
                    byte[] layout = patch.ExtendLayout(File.ReadAllBytes(args[2]));
                    Check(layout.SequenceEqual(File.ReadAllBytes(args[3])), "Extended layout differs from the verified ELF");
                    byte[] image = File.ReadAllBytes(args[4]);
                    OfflinePatch.Overlay(image, layout, patch.Writes);
                    Check(image.SequenceEqual(File.ReadAllBytes(args[5])), "Offline mode patch differs from the verified GT7.exe");
                }
                // FEAT-GT7-181: the renderer's switch names, and the DLSS texture detail's variables.
                Func<string, byte[]> bytes = text => System.Text.Encoding.ASCII.GetBytes(text);
                Check(LoaderForm.NamesSwitch(bytes("xx\0RYGT_A\0yy"), "RYGT_A"), "A switch name inside the file is found");
                Check(LoaderForm.NamesSwitch(bytes("RYGT_A\0"), "RYGT_A"), "A switch name at the start of the file is found");
                Check(!LoaderForm.NamesSwitch(bytes("xx\0RYGT_A"), "RYGT_A"), "A name cut at the end of the file is not a switch");
                Check(!LoaderForm.NamesSwitch(bytes("\0RYGT_AB\0"), "RYGT_A"), "The start of a longer name is not that switch");
                Check(!LoaderForm.NamesSwitch(bytes("\0XRYGT_A\0"), "RYGT_A"), "The end of a longer name is not that switch");
                Check(!LoaderForm.NamesSwitch(new byte[0], "RYGT_A"), "An empty file names no switch");
                string detailBias, detailExtra;
                LoaderForm.DlssDetailVariables("full", out detailBias, out detailExtra);
                Check(detailBias == null && detailExtra == null, "DLSS texture detail Full sets no variable");
                LoaderForm.DlssDetailVariables(null, out detailBias, out detailExtra);
                Check(detailBias == null && detailExtra == null, "A missing DLSS texture detail sets no variable");
                LoaderForm.DlssDetailVariables("Reduced", out detailBias, out detailExtra);
                Check(detailBias == null && detailExtra == null, "An unknown DLSS texture detail is Full, as the page shows it");
                LoaderForm.DlssDetailVariables("reduced", out detailBias, out detailExtra);
                Check(detailBias == "1" && detailExtra == "0", "DLSS texture detail Reduced: the bias without its extra level");
                LoaderForm.DlssDetailVariables("off", out detailBias, out detailExtra);
                Check(detailBias == "0" && detailExtra == null, "DLSS texture detail Off: no bias");
                TrophyChecks();
                TrophyFileChecks(Path.Combine(root, "trophy-files"));
                Console.WriteLine("PASS: " + checks + " GT7 setup checks");
                return 0;
            }
            catch (Exception e) { Console.Error.WriteLine(e); return 1; }
            // Test files remain under the temporary folder for inspection; never remove game dumps.
        }
    }
}
