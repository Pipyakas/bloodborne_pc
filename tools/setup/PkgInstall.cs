// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: installs PS4 .pkg files (fake-signed game and update packages) into a game folder, for
// the setup program and bbport-pkg.exe (PkgTool.cs). A C# 5 port of the Rust crates orbis-pkg,
// orbis-pfs and orbis-pkg-util 0.1.0 (https://github.com/obhq, MIT OR Apache-2.0), which
// shadps4-game-manager installs packages with:
//   PKG: big-endian header and entry table; entry key 3 (RSA, a public fake key) decrypts the
//        entries' AES-CBC keys; the PFS image key (EKPFS) is RSA-encrypted with another one.
//   PFS: the outer image is AES-XTS encrypted (keys from HMAC-SHA256(EKPFS, seed)) per 4 KiB
//        sector; its uroot/pfs_image.dat is the inner image (PFSC: zlib blocks), whose uroot
//        holds the game files. sce_sys comes from the PKG entries.
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Numerics;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace BbportSetup {

/// The PS4 param.sfo key/value table: magic "\0PSF", then the key and data table offsets and
/// 16-byte entries (key offset, format, length, max length, data offset).
static class ParamSfo {
    public static Dictionary<string, string> Parse(byte[] d) {
        if (d == null || d.Length < 20 || d[0] != 0 || d[1] != (byte)'P' || d[2] != (byte)'S' || d[3] != (byte)'F') return null;
        int keys = BitConverter.ToInt32(d, 8), data = BitConverter.ToInt32(d, 12), count = BitConverter.ToInt32(d, 16);
        var result = new Dictionary<string, string>();
        for (int i = 0; i < count; ++i) {
            int e = 20 + i * 16;
            int keyOffset = BitConverter.ToUInt16(d, e), format = BitConverter.ToUInt16(d, e + 2);
            int length = BitConverter.ToInt32(d, e + 4), dataOffset = BitConverter.ToInt32(d, e + 12);
            int k = keys + keyOffset, end = k;
            while (end < d.Length && d[end] != 0) ++end;
            string key = Encoding.ASCII.GetString(d, k, end - k);
            if (format == 0x0204 || format == 0x0004) {
                result[key] = Encoding.UTF8.GetString(d, data + dataOffset, length).TrimEnd('\0');
            } else if (format == 0x0404) {
                result[key] = BitConverter.ToUInt32(d, data + dataOffset).ToString();
            }
        }
        return result;
    }

    public static Dictionary<string, string> Read(string path) {
        try {
            return Parse(File.ReadAllBytes(path));
        } catch (Exception) {
            return null;
        }
    }

    public static string Get(Dictionary<string, string> sfo, string key) {
        string value;
        return sfo != null && sfo.TryGetValue(key, out value) ? value : null;
    }
}

class PkgException : Exception {
    public PkgException(string message) : base(message) { }
}

/// One entry of the PKG's entry table (32 bytes, big-endian).
class PkgEntry {
    public byte[] Raw;
    public uint Id { get { return Be.U32(Raw, 0); } }
    public bool Encrypted { get { return (Be.U32(Raw, 8) & 0x80000000u) != 0; } }
    public int KeyIndex { get { return (int)((Be.U32(Raw, 12) & 0xf000) >> 12); } }
    public long Offset { get { return Be.U32(Raw, 16); } }
    public int Size { get { return (int)Be.U32(Raw, 20); } }
}

static class Be {
    public static uint U32(byte[] b, int o) { return (uint)(b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3]); }
    public static ulong U64(byte[] b, int o) { return (ulong)U32(b, o) << 32 | U32(b, o + 4); }
}

/// A PKG file: header, entries and the keys of its PFS image.
sealed class PkgFile : IDisposable {
    public readonly string Path;
    public readonly long Length;
    public string ContentId, TitleId;
    public uint ContentFlags;
    public long PfsOffset, PfsSize;
    public readonly List<PkgEntry> Entries = new List<PkgEntry>();
    public Dictionary<string, string> Sfo;
    public byte[] Ekpfs;
    byte[] entryKey3;
    readonly FileStream stream;

    public const uint EntryKeys = 0x10, PfsImageKey = 0x20, ParamSfoId = 0x1000;

    PkgFile(string path) {
        Path = path;
        stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16, FileOptions.RandomAccess);
        Length = stream.Length;
    }

    public static PkgFile Open(string path) {
        var pkg = new PkgFile(path);
        try {
            pkg.Load();
            return pkg;
        } catch {
            pkg.Dispose();
            throw;
        }
    }

    public void Dispose() { stream.Dispose(); }

    /// Game ("gd"), update ("gp"), add-on ("ac"); null when unknown.
    public string Category { get { return ParamSfo.Get(Sfo, "CATEGORY"); } }
    public string AppVersion { get { return ParamSfo.Get(Sfo, "APP_VER"); } }
    public string Title { get { return ParamSfo.Get(Sfo, "TITLE"); } }
    /// Delta updates hold binary differences instead of whole files.
    public bool DeltaPatch { get { return (ContentFlags & 0x41000000u) == 0x41000000u; } }

    byte[] ReadAt(long offset, int count) {
        if (offset < 0 || offset + count > Length) throw new PkgException("the package is truncated (damaged or incomplete download)");
        var buffer = new byte[count];
        stream.Position = offset;
        int done = 0;
        while (done < count) {
            int n = stream.Read(buffer, done, count - done);
            if (n <= 0) throw new PkgException("the package is truncated");
            done += n;
        }
        return buffer;
    }

    void Load() {
        if (Length < 0x1000) throw new PkgException("not a PS4 package (too small)");
        byte[] h = ReadAt(0, 0x1000);
        if (Be.U32(h, 0) != 0x7F434E54) throw new PkgException("not a PS4 package (no CNT magic)");
        uint entryCount = Be.U32(h, 0x10), tableOffset = Be.U32(h, 0x18);
        ContentId = Encoding.ASCII.GetString(h, 0x40, 0x24).TrimEnd('\0');
        TitleId = ContentId.Length >= 16 ? ContentId.Substring(7, 9) : "";
        ContentFlags = Be.U32(h, 0x78);
        PfsOffset = (long)Be.U64(h, 0x410);
        PfsSize = (long)Be.U64(h, 0x418);
        if (entryCount > 100000) throw new PkgException("invalid entry table");
        byte[] table = ReadAt(tableOffset, (int)entryCount * 32);
        for (int i = 0; i < entryCount; ++i) {
            var raw = new byte[32];
            Buffer.BlockCopy(table, i * 32, raw, 0, 32);
            Entries.Add(new PkgEntry { Raw = raw });
        }
        // Entry key 3: entry_keys holds a seed, 7 digests and 7 RSA-encrypted keys.
        PkgEntry keys = Find(EntryKeys);
        if (keys == null) throw new PkgException("no entry keys in the package");
        byte[] keyData = ReadAt(keys.Offset, keys.Size);
        if (keyData.Length < 32 + 7 * 32 + 7 * 256) throw new PkgException("invalid entry keys");
        var key3 = new byte[256];
        Buffer.BlockCopy(keyData, 32 + 7 * 32 + 3 * 256, key3, 0, 256);
        entryKey3 = PkgKeys.Decrypt(PkgKeys.Key3, key3);
        PkgEntry imageKey = Find(PfsImageKey);
        if (imageKey == null) throw new PkgException("no PFS image key in the package");
        Ekpfs = PkgKeys.Decrypt(PkgKeys.FakePfs, EntryData(imageKey));
        PkgEntry sfo = Find(ParamSfoId);
        if (sfo != null) Sfo = ParamSfo.Parse(EntryData(sfo));
    }

    public PkgEntry Find(uint id) {
        foreach (PkgEntry e in Entries) if (e.Id == id) return e;
        return null;
    }

    /// The entry's data, decrypted; null when it is encrypted with a key other than key 3.
    public byte[] EntryData(PkgEntry entry) {
        if (!entry.Encrypted) return ReadAt(entry.Offset, entry.Size);
        if (entry.KeyIndex != 3) return null;
        byte[] data = ReadAt(entry.Offset, (entry.Size + 15) & ~15);
        // Key and IV: SHA-256(raw entry || entry key 3) = IV (16) || key (16).
        byte[] secret;
        using (var sha = SHA256.Create()) {
            var input = new byte[32 + entryKey3.Length];
            Buffer.BlockCopy(entry.Raw, 0, input, 0, 32);
            Buffer.BlockCopy(entryKey3, 0, input, 32, entryKey3.Length);
            secret = sha.ComputeHash(input);
        }
        var iv = new byte[16];
        var key = new byte[16];
        Buffer.BlockCopy(secret, 0, iv, 0, 16);
        Buffer.BlockCopy(secret, 16, key, 0, 16);
        using (var aes = new AesCryptoServiceProvider { Mode = CipherMode.CBC, Padding = PaddingMode.None, Key = key, IV = iv })
        using (ICryptoTransform decryptor = aes.CreateDecryptor()) {
            byte[] plain = decryptor.TransformFinalBlock(data, 0, data.Length);
            Array.Resize(ref plain, entry.Size);
            return plain;
        }
    }

    /// Where an entry goes under sce_sys (orbis-pkg EntryId::to_path); null for the package's
    /// own metadata (keys, digests) and unknown entries.
    public static string EntryFileName(uint id) {
        switch (id) {
        case 0x400: return "license.dat";
        case 0x401: return "license.info";
        case 0x402: return "nptitle.dat";
        case 0x403: return "npbind.dat";
        case 0x404: return "selfinfo.dat";
        case 0x406: return "imageinfo.dat";
        case 0x407: return "target-deltainfo.dat";
        case 0x408: return "origin-deltainfo.dat";
        case 0x409: return "psreserved.dat";
        case 0x1000: return "param.sfo";
        case 0x1001: return "playgo-chunk.dat";
        case 0x1002: return "playgo-chunk.sha";
        case 0x1003: return "playgo-manifest.xml";
        case 0x1004: return "pronunciation.xml";
        case 0x1005: return "pronunciation.sig";
        case 0x1006: return "pic1.png";
        case 0x1007: return "pubtoolinfo.dat";
        case 0x1008: return @"app\playgo-chunk.dat";
        case 0x1009: return @"app\playgo-chunk.sha";
        case 0x100a: return @"app\playgo-manifest.xml";
        case 0x100b: return "shareparam.json";
        case 0x100c: return "shareoverlayimage.png";
        case 0x100d: return "save_data.png";
        case 0x100e: return "shareprivacyguardimage.png";
        case 0x1200: return "icon0.png";
        case 0x1220: return "pic0.png";
        case 0x1240: return "snd0.at9";
        case 0x1260: return @"changeinfo\changeinfo.xml";
        case 0x1280: return "icon0.dds";
        case 0x12a0: return "pic0.dds";
        case 0x12c0: return "pic1.dds";
        }
        if (id >= 0x1201 && id <= 0x121f) return string.Format("icon0_{0:D2}.png", id - 0x1201);
        if (id >= 0x1241 && id <= 0x125f) return string.Format("pic1_{0:D2}.png", id - 0x1241);
        if (id >= 0x1261 && id <= 0x127f) return string.Format(@"changeinfo\changeinfo_{0:D2}.xml", id - 0x1261);
        if (id >= 0x1281 && id <= 0x129f) return string.Format("icon0_{0:D2}.dds", id - 0x1281);
        if (id >= 0x12c1 && id <= 0x12df) return string.Format("pic1_{0:D2}.dds", id - 0x12c1);
        if (id >= 0x1400 && id <= 0x1463) return string.Format(@"trophy\trophy{0:D2}.trp", id - 0x1400);
        return null;
    }
}

/// The RSA keys every fake-signed package uses (orbis-pkg keys.rs: modulus and private exponent).
static class PkgKeys {
    public sealed class Key { public BigInteger N, D; }

    public static readonly Key Key3 = Make(
        "d212fc335f6ddb831609628b0356273782d477853529392d526b8c4c8cfb06c1845be7d4f7bcd24e6245cd2abbd777764536" +
        "55273fb3f5f98eda4befaa59aeb39bea5498d206326a58312ae0d44f90b50a7decf43a9c52672d99318e0c43e682fe0746e1" +
        "2e50d41f2d2f7ed908ba06b3bf2e203f4e3ffe44ffaa50435791699449158282e40f4c8d9d2cc95b1d64bf888bd4c594e765" +
        "47841ee57910fb989347b97d8512a640982cf792bc951932ede890560d65c1aa78c62e54fd5f54a1f67ee5e05f61c120b4b9" +
        "b4330870e4df8956ed012946775f8cb8a9f51e2eb3b9bfe009b78d28d4a6c3b81e1f07ebb4120b95b88530fddc3913d07cdc" +
        "8fedf9c9a3c1",
        "32d903908fbdb08f572b285e0b8db3ea5cd17ea890888cdd6a80bbb1dfc1f70daa32f0b77ccb88800e8b64b0be4cd60e9b8c" +
        "1e2a64e1f35cd77601415e935c94fedd4662c31b5ae2a0bc2debc3980aa7b7856970682b644ab31fcc7ddc7c26f477f65cf2" +
        "ae5a442dd3ab16620419bafb90ffe23050896ecb56b2ebc09116925e308eaec7945dfd35e120f8ad3ebc08bfc036749fd5bb" +
        "5208fd0666f37ab304f475295de95faa1030b20f5a1ac12ab3fecb21ad80ec8f20091cdbc55894c29cc6ce82653e5790bca9" +
        "8b06b4f072f677df9864f1ecfe372dbcae8c08811fc3c9891ac742824b2edc8e8d73ceb1cc01d90870873c4408ec498f815a" +
        "e240ff77fc0d");

    public static readonly Key FakePfs = Make(
        "c6cf71e7e59af0d12a2c458bf92a0ec143058bc37117801dcd497dde359d259ba0d7a0f27d6c087eaa5502682b23c644b844" +
        "18eb56cf16a24803c9e74f87eb3d30c31588bf20e79dff770cde1d241e63a94f8abf5bbe601968333bfced9f474e5ff8eacb" +
        "3d00bd6701f92c6dc6ac1364e76714f3dc52696ab9832c4230131bb2d8a5020d79ed96b10df8cc0cdf81954f035809570e80" +
        "692efeff5277ea7528a8fbc9bebf9fbbb7798e1805e180bd50349481d353c269a2d24ccf6cf4572c104a3ffb22fd8b97e2c9" +
        "5ba62bcdd61b6bdb687f4bc2a05034c005e58def2467ff9340cf2d62a2a050b1f13aa83dfd80d1f9b80522afc8354590588e" +
        "e33a7cbd3e27",
        "7f76cd0ee2d4de051cc6d9a80e8dfa7bca1eaa271a40f8f1228735dddbfdeef8c2bcbd01fb8be23e63b2b1225c56496e11be" +
        "07440b9a2666d1492c8fd31bcfa4a1b8d1fba49ed2212883098af6a00ba3d60f9b6368ccbc0c4e145b27a4a9f42bb9b87bc0" +
        "e651ad1d77d46bb9ce20d126667e5e9ea2e96b90f373b8528f4411030c1397393d132258d5438249da6e7ca1c58ca5b009e0" +
        "ce3ddff49d3c9715e26ac72b3c509323dbba4a226644ac78bb0e1a2743b57167aff4ab48469373d042ab9363e56c9ade5024" +
        "c0237d99793f2207e0c148561bdf830912b42d456bc9c06885999079961ad7f54d1f3783404aec3937a680927dc580c7d66f" +
        "fe8a7989c6b1");

    static Key Make(string n, string d) { return new Key { N = Number(n), D = Number(d) }; }

    static BigInteger Number(string hex) {
        // BigInteger wants little-endian bytes with a clear sign bit.
        var bytes = new byte[hex.Length / 2 + 1];
        for (int i = 0; i < hex.Length / 2; ++i) bytes[hex.Length / 2 - 1 - i] = Convert.ToByte(hex.Substring(i * 2, 2), 16);
        return new BigInteger(bytes);
    }

    /// RSA decryption with PKCS #1 v1.5 padding (00 02 nonzero-bytes 00 message).
    public static byte[] Decrypt(Key key, byte[] cipher) {
        var le = new byte[cipher.Length + 1];
        for (int i = 0; i < cipher.Length; ++i) le[cipher.Length - 1 - i] = cipher[i];
        byte[] m = BigInteger.ModPow(new BigInteger(le), key.D, key.N).ToByteArray();
        var be = new byte[cipher.Length];
        for (int i = 0; i < m.Length && i < be.Length; ++i) be[be.Length - 1 - i] = m[i];
        if (be[0] != 0 || be[1] != 2) throw new PkgException("cannot decrypt the package keys (not a fake-signed package?)");
        int zero = Array.IndexOf<byte>(be, 0, 2);
        if (zero < 10) throw new PkgException("cannot decrypt the package keys (bad padding)");
        var message = new byte[be.Length - zero - 1];
        Buffer.BlockCopy(be, zero + 1, message, 0, message.Length);
        return message;
    }
}

/// Random-access bytes; Read must return exactly count bytes and be safe to call from several
/// threads at once.
interface IImage {
    long Length { get; }
    void Read(long offset, byte[] buffer, int index, int count);
}

/// The PKG's PFS image, decrypted per 4 KiB XTS sector (the first block, the header, is plain).
sealed class XtsImage : IImage, IDisposable {
    const int Sector = 0x1000, Batch = 256;
    readonly string path;
    readonly long start, length, encryptedFrom;
    readonly byte[] dataKey, tweakKey;
    readonly ThreadLocal<State> states;
    readonly List<State> all = new List<State>();

    sealed class State {
        public FileStream File;
        public ICryptoTransform Data, Tweak;
        public byte[] Raw = new byte[Sector * Batch], Plain = new byte[Sector], Tweaks = new byte[Sector], T = new byte[16], S = new byte[16];
    }

    public XtsImage(string path, long start, long length, int blockSize, byte[] ekpfs, byte[] seed) {
        this.path = path;
        this.start = start;
        this.length = length;
        encryptedFrom = blockSize / Sector;
        // HMAC-SHA256(EKPFS, 01 00 00 00 || seed) = tweak key (16) || data key (16).
        byte[] secret;
        using (var hmac = new HMACSHA256(ekpfs)) {
            var input = new byte[20];
            input[0] = 1;
            Buffer.BlockCopy(seed, 0, input, 4, 16);
            secret = hmac.ComputeHash(input);
        }
        tweakKey = new byte[16];
        dataKey = new byte[16];
        Buffer.BlockCopy(secret, 0, tweakKey, 0, 16);
        Buffer.BlockCopy(secret, 16, dataKey, 0, 16);
        states = new ThreadLocal<State>(NewState);
    }

    State NewState() {
        var s = new State {
            File = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1, FileOptions.RandomAccess),
            Data = Ecb(dataKey).CreateDecryptor(),
            Tweak = Ecb(tweakKey).CreateEncryptor(),
        };
        lock (all) all.Add(s);
        return s;
    }

    static Aes Ecb(byte[] key) {
        return new AesCryptoServiceProvider { Mode = CipherMode.ECB, Padding = PaddingMode.None, Key = key };
    }

    public long Length { get { return length; } }

    public void Dispose() {
        lock (all) foreach (State s in all) { s.File.Dispose(); s.Data.Dispose(); s.Tweak.Dispose(); }
        states.Dispose();
    }

    public void Read(long offset, byte[] buffer, int index, int count) {
        if (offset < 0 || offset + count > length) throw new PkgException("PFS read outside the image");
        State s = states.Value;
        while (count > 0) {
            long first = offset / Sector;
            int within = (int)(offset % Sector);
            int sectors = (int)Math.Min(Batch, (within + count + Sector - 1) / Sector);
            int bytes = (int)Math.Min((long)sectors * Sector, length - first * Sector);
            s.File.Position = start + first * Sector;
            int done = 0;
            while (done < bytes) {
                int n = s.File.Read(s.Raw, done, bytes - done);
                if (n <= 0) throw new PkgException("the package is truncated (PFS image)");
                done += n;
            }
            for (int i = 0; i * Sector < bytes; ++i) {
                if (first + i >= encryptedFrom) DecryptSector(s, first + i, i * Sector);
            }
            int n2 = Math.Min(count, bytes - within);
            Buffer.BlockCopy(s.Raw, within, buffer, index, n2);
            offset += n2;
            index += n2;
            count -= n2;
        }
    }

    /// XTS-AES-128: T = E(tweak key, sector number), then P = D(data key, C ^ T) ^ T with T
    /// multiplied by x in GF(2^128) per 16-byte block.
    static void DecryptSector(State s, long sector, int at) {
        Array.Clear(s.S, 0, 16);
        for (int i = 0; i < 8; ++i) s.S[i] = (byte)(sector >> (8 * i));
        s.Tweak.TransformBlock(s.S, 0, 16, s.T, 0);
        byte[] raw = s.Raw, tweaks = s.Tweaks, t = s.T;
        for (int b = 0; b < Sector; b += 16) {
            Buffer.BlockCopy(t, 0, tweaks, b, 16);
            for (int i = 0; i < 16; ++i) raw[at + b + i] ^= t[i];
            int carry = t[15] >> 7;
            for (int i = 15; i > 0; --i) t[i] = (byte)(t[i] << 1 | t[i - 1] >> 7);
            t[0] = (byte)(t[0] << 1 ^ (carry != 0 ? 0x87 : 0));
        }
        s.Data.TransformBlock(raw, at, Sector, s.Plain, 0);
        for (int i = 0; i < Sector; ++i) raw[at + i] = (byte)(s.Plain[i] ^ tweaks[i]);
    }
}

/// A PFS (the PS4's package file system): little-endian header, inodes from block 1 on,
/// directories as dirent lists, files by block maps.
sealed class Pfs {
    public sealed class Inode {
        public ushort Mode;
        public uint Flags, Blocks;
        public long Size;
        public uint[] Direct = new uint[12], Indirect = new uint[5];
        public uint[] Map;
        public bool Compressed { get { return (Flags & 1) != 0; } }
    }

    public sealed class Item {
        public string Name;
        public int Inode;
        public bool Directory;
    }

    readonly IImage image;
    public readonly int BlockSize;
    public readonly Inode[] Inodes;
    public readonly int SuperRoot;
    readonly bool signed;

    public const int HeaderSize = 0x380;

    public static int ModeOf(byte[] header) { return BitConverter.ToUInt16(header, 0x1C); }
    public static int BlockSizeOf(byte[] header) { return BitConverter.ToInt32(header, 0x20); }

    public static byte[] KeySeed(byte[] header) {
        var seed = new byte[16];
        Buffer.BlockCopy(header, 0x370, seed, 0, 16);
        return seed;
    }

    public static void Check(byte[] header) {
        if (BitConverter.ToUInt64(header, 0) != 1 || BitConverter.ToUInt64(header, 8) != 20130315)
            throw new PkgException("invalid PFS header");
    }

    public Pfs(IImage image) {
        this.image = image;
        var header = new byte[HeaderSize];
        image.Read(0, header, 0, HeaderSize);
        Check(header);
        int mode = ModeOf(header);
        signed = (mode & 1) != 0;
        BlockSize = BlockSizeOf(header);
        if (BlockSize <= 0 || (BlockSize & (BlockSize - 1)) != 0) throw new PkgException("invalid PFS block size");
        long inodeCount = (long)BitConverter.ToUInt64(header, 0x30);
        long inodeBlocks = (long)BitConverter.ToUInt64(header, 0x40);
        SuperRoot = (int)BitConverter.ToUInt64(header, 0x48);
        var inodes = new List<Inode>();
        int raw = 100 + (signed ? 612 : 68);
        var block = new byte[BlockSize];
        for (long b = 0; b < inodeBlocks && inodes.Count < inodeCount; ++b) {
            image.Read((1 + b) * BlockSize, block, 0, BlockSize);
            for (int o = 0; o + raw <= BlockSize && inodes.Count < inodeCount; o += raw) inodes.Add(ParseInode(block, o));
        }
        Inodes = inodes.ToArray();
        if (SuperRoot >= Inodes.Length) throw new PkgException("invalid PFS super-root");
        foreach (Inode inode in Inodes) inode.Map = BlockMap(inode);
    }

    Inode ParseInode(byte[] b, int o) {
        var inode = new Inode {
            Mode = BitConverter.ToUInt16(b, o),
            Flags = BitConverter.ToUInt32(b, o + 4),
            Size = BitConverter.ToInt64(b, o + 8),
            Blocks = BitConverter.ToUInt32(b, o + 96),
        };
        int p = o + 100;
        // Signed images precede each block pointer with a 32-byte signature.
        int stride = signed ? 36 : 4, skip = signed ? 32 : 0;
        for (int i = 0; i < 12; ++i, p += stride) inode.Direct[i] = BitConverter.ToUInt32(b, p + skip);
        for (int i = 0; i < 5; ++i, p += stride) inode.Indirect[i] = BitConverter.ToUInt32(b, p + skip);
        return inode;
    }

    uint[] BlockMap(Inode inode) {
        long count = inode.Blocks;
        var map = new List<uint>((int)Math.Min(count, 1 << 20));
        if (count == 0) return map.ToArray();
        if (inode.Direct[1] == 0xffffffff) { // contiguous
            for (long i = 0; i < count; ++i) map.Add((uint)(inode.Direct[0] + i));
            return map.ToArray();
        }
        for (int i = 0; i < 12 && map.Count < count; ++i) map.Add(inode.Direct[i]);
        if (map.Count == count) return map.ToArray();
        var level0 = new byte[BlockSize];
        image.Read((long)inode.Indirect[0] * BlockSize, level0, 0, BlockSize);
        foreach (uint v in Pointers(level0)) {
            map.Add(v);
            if (map.Count == count) return map.ToArray();
        }
        image.Read((long)inode.Indirect[1] * BlockSize, level0, 0, BlockSize);
        var level1 = new byte[BlockSize];
        foreach (uint v in Pointers(level0)) {
            image.Read((long)v * BlockSize, level1, 0, BlockSize);
            foreach (uint w in Pointers(level1)) {
                map.Add(w);
                if (map.Count == count) return map.ToArray();
            }
        }
        throw new PkgException("PFS file too large (triple indirect blocks)");
    }

    IEnumerable<uint> Pointers(byte[] block) {
        int stride = signed ? 36 : 4, skip = signed ? 32 : 0;
        for (int o = 0; o + stride <= block.Length; o += stride) yield return BitConverter.ToUInt32(block, o + skip);
    }

    public List<Item> List(int dir) {
        var items = new List<Item>();
        var block = new byte[BlockSize];
        foreach (uint b in Inodes[dir].Map) {
            image.Read((long)b * BlockSize, block, 0, BlockSize);
            for (int o = 0; o + 16 <= BlockSize; ) {
                int ino = BitConverter.ToInt32(block, o), type = BitConverter.ToInt32(block, o + 4);
                int nameLength = BitConverter.ToInt32(block, o + 8), size = BitConverter.ToInt32(block, o + 12);
                if (size == 0) break;
                if (o + 16 + nameLength > BlockSize || size < 16 + nameLength) throw new PkgException("invalid PFS directory entry");
                string name = Encoding.UTF8.GetString(block, o + 16, nameLength);
                o += size;
                if (type == 4 || type == 5) continue; // . and ..
                if (type != 2 && type != 3) throw new PkgException("unknown PFS directory entry type " + type);
                if (ino < 0 || ino >= Inodes.Length) throw new PkgException("invalid PFS inode " + ino);
                items.Add(new Item { Name = name, Inode = ino, Directory = type == 3 });
            }
        }
        return items;
    }

    public Item Find(int dir, string name) {
        foreach (Item item in List(dir)) if (item.Name == name) return item;
        return null;
    }

    public void ReadFile(int inode, long offset, byte[] buffer, int index, int count) {
        Inode node = Inodes[inode];
        if (offset < 0 || offset + count > node.Size) throw new PkgException("PFS read past the end of a file");
        while (count > 0) {
            long block = offset / BlockSize;
            int within = (int)(offset % BlockSize);
            // Merge physically consecutive blocks into one read.
            int n = BlockSize - within;
            long b = block;
            while (n < count && b + 1 < node.Map.Length && node.Map[b + 1] == node.Map[b] + 1) { ++b; n += BlockSize; }
            n = Math.Min(n, count);
            image.Read((long)node.Map[block] * BlockSize + within, buffer, index, n);
            offset += n;
            index += n;
            count -= n;
        }
    }
}

/// A file inside a PFS, as an image (the inner pfs_image.dat).
sealed class PfsFileImage : IImage {
    readonly Pfs pfs;
    readonly int inode;
    public PfsFileImage(Pfs pfs, int inode) { this.pfs = pfs; this.inode = inode; }
    public long Length { get { return pfs.Inodes[inode].Size; } }
    public void Read(long offset, byte[] buffer, int index, int count) { pfs.ReadFile(inode, offset, buffer, index, count); }
}

/// PFSC: an image stored as zlib-compressed blocks with an offset table.
sealed class PfscImage : IImage {
    readonly IImage source;
    readonly int blockSize;
    readonly long originalBlockSize, length;
    readonly long[] offsets;

    public PfscImage(IImage source) {
        this.source = source;
        var h = new byte[0x30];
        source.Read(0, h, 0, h.Length);
        if (h[0] != 'P' || h[1] != 'F' || h[2] != 'S' || h[3] != 'C') throw new PkgException("invalid PFSC image");
        blockSize = BitConverter.ToInt32(h, 0x0C);
        originalBlockSize = BitConverter.ToInt64(h, 0x10);
        long table = BitConverter.ToInt64(h, 0x18);
        length = BitConverter.ToInt64(h, 0x28);
        long count = length / originalBlockSize + 1;
        var raw = new byte[count * 8];
        source.Read(table, raw, 0, raw.Length);
        offsets = new long[count];
        for (int i = 0; i < count; ++i) offsets[i] = BitConverter.ToInt64(raw, i * 8);
    }

    public long Length { get { return length; } }

    void Block(long num, byte[] output) {
        long offset = offsets[num], size = offsets[num + 1] - offset;
        if (size > originalBlockSize) { Array.Clear(output, 0, blockSize); return; } // sparse
        if (size == originalBlockSize) { source.Read(offset, output, 0, blockSize); return; }
        var packed = new byte[size];
        source.Read(offset, packed, 0, (int)size);
        // zlib: skip the 2-byte header; DeflateStream reads the raw stream.
        using (var inflate = new DeflateStream(new MemoryStream(packed, 2, (int)size - 2), CompressionMode.Decompress)) {
            int done = 0;
            while (done < blockSize) {
                int n = inflate.Read(output, done, blockSize - done);
                if (n <= 0) break;
                done += n;
            }
            if (done != blockSize) throw new PkgException("invalid data in PFSC block " + num);
        }
    }

    public void Read(long offset, byte[] buffer, int index, int count) {
        if (offset < 0 || offset + count > length) throw new PkgException("PFSC read outside the image");
        var block = new byte[blockSize];
        while (count > 0) {
            long num = offset / blockSize;
            int within = (int)(offset % blockSize);
            Block(num, block);
            int n = Math.Min(count, blockSize - within);
            Buffer.BlockCopy(block, within, buffer, index, n);
            offset += n;
            index += n;
            count -= n;
        }
    }
}

/// Extracts and installs packages.
static class PkgInstaller {
    /// Called from worker threads: bytes written so far, total bytes, the file being written.
    public delegate void Progress(long done, long total, string file);

    sealed class Work {
        public int Inode;
        public string Output;
        public long Size;
    }

    /// sce_sys from the entries, then the game files from the PFS image, into output.
    public static void Extract(PkgFile pkg, string output, Action<string> log, Progress progress) {
        string sceSys = Path.Combine(output, "sce_sys");
        int written = 0, skipped = 0;
        foreach (PkgEntry entry in pkg.Entries) {
            string name = PkgFile.EntryFileName(entry.Id);
            if (name == null) continue;
            byte[] data = pkg.EntryData(entry);
            if (data == null) { ++skipped; continue; }
            string path = Path.Combine(sceSys, name);
            Directory.CreateDirectory(Path.GetDirectoryName(path));
            File.WriteAllBytes(path, data);
            ++written;
        }
        log(string.Format("sce_sys: {0} files ({1} skipped: no key)", written, skipped));

        if (pkg.PfsOffset <= 0 || pkg.PfsSize <= 0 || pkg.PfsOffset + pkg.PfsSize > pkg.Length)
            throw new PkgException("the package has no PFS image (or is truncated)");
        byte[] header = new byte[Pfs.HeaderSize];
        using (var file = new FileStream(pkg.Path, FileMode.Open, FileAccess.Read, FileShare.Read)) {
            file.Position = pkg.PfsOffset;
            if (file.Read(header, 0, header.Length) != header.Length) throw new PkgException("the package is truncated");
        }
        Pfs.Check(header);
        if ((Pfs.ModeOf(header) & 4) == 0) throw new PkgException("unencrypted outer PFS images are not supported");
        using (var outerImage = new XtsImage(pkg.Path, pkg.PfsOffset, pkg.PfsSize, Pfs.BlockSizeOf(header), pkg.Ekpfs, Pfs.KeySeed(header))) {
            var outer = new Pfs(outerImage);
            Pfs.Item uroot = outer.Find(outer.SuperRoot, "uroot");
            if (uroot == null || !uroot.Directory) throw new PkgException("no uroot in the outer PFS");
            Pfs.Item imageFile = outer.Find(uroot.Inode, "pfs_image.dat");
            if (imageFile == null || imageFile.Directory) throw new PkgException("no pfs_image.dat in the package");
            IImage innerImage = new PfsFileImage(outer, imageFile.Inode);
            if (outer.Inodes[imageFile.Inode].Compressed) innerImage = new PfscImage(innerImage);
            var inner = new Pfs(innerImage);
            Pfs.Item root = inner.Find(inner.SuperRoot, "uroot");
            if (root == null || !root.Directory) throw new PkgException("no uroot in the inner PFS");

            var dirs = new List<string>();
            var files = new List<Work>();
            Collect(inner, root.Inode, output, dirs, files);
            long total = files.Sum(f => f.Size);
            EnsureSpace(output, total);
            log(string.Format("PFS: {0} files, {1:F1} GB", files.Count, total / 1e9));
            foreach (string dir in dirs) Directory.CreateDirectory(dir);
            long done = 0;
            // Largest first: the big archives start early and the threads finish together.
            files.Sort((a, b) => b.Size.CompareTo(a.Size));
            var options = new ParallelOptions { MaxDegreeOfParallelism = Math.Max(2, Math.Min(8, Environment.ProcessorCount)) };
            Parallel.ForEach(files, options, () => new byte[4 << 20], (work, state, buffer) => {
                using (var dest = new FileStream(work.Output, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 16)) {
                    dest.SetLength(work.Size);
                    for (long at = 0; at < work.Size; ) {
                        int n = (int)Math.Min(buffer.Length, work.Size - at);
                        inner.ReadFile(work.Inode, at, buffer, 0, n);
                        dest.Write(buffer, 0, n);
                        at += n;
                        if (progress != null) progress(Interlocked.Add(ref done, n), total, work.Output);
                    }
                }
                return buffer;
            }, buffer => { });
        }
    }

    static void Collect(Pfs pfs, int dir, string output, List<string> dirs, List<Work> files) {
        foreach (Pfs.Item item in pfs.List(dir)) {
            if (item.Name.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 || item.Name == "." || item.Name == "..")
                throw new PkgException("unsupported file name in the package: " + item.Name);
            string path = Path.Combine(output, item.Name);
            if (item.Directory) {
                dirs.Add(path);
                Collect(pfs, item.Inode, path, dirs, files);
            } else {
                files.Add(new Work { Inode = item.Inode, Output = path, Size = pfs.Inodes[item.Inode].Size });
            }
        }
    }

    static void EnsureSpace(string output, long bytes) {
        string root = Path.GetPathRoot(Path.GetFullPath(output));
        try {
            long free = new DriveInfo(root).AvailableFreeSpace;
            if (free < bytes + (256L << 20))
                throw new PkgException(string.Format("not enough free space on {0}: {1:F1} GB needed, {2:F1} GB free", root, bytes / 1e9, free / 1e9));
        } catch (ArgumentException) {
            // Network paths: no drive information.
        }
    }

    public static string Describe(PkgFile pkg) {
        string kind = pkg.Category == "gd" ? "game" : pkg.Category == "gp" ? "update" : pkg.Category == "ac" ? "add-on" : "category " + (pkg.Category ?? "?");
        return string.Format("{0} {1} v{2} ({3}{4}, {5:F1} GB)", pkg.Title ?? "?", pkg.TitleId, pkg.AppVersion ?? "?", kind,
                             pkg.DeltaPatch ? ", delta" : "", pkg.Length / 1e9);
    }

    /// Installs a game package and its updates into gameRoot\TITLE_ID (updates replace the files
    /// they contain, as the console does), and returns that folder. The game goes to a
    /// .partial folder first and is renamed when complete; an existing install is updated.
    public static string Install(IList<string> packages, string gameRoot, Action<string> log, Progress progress) {
        var pkgs = new List<PkgFile>();
        try {
            foreach (string path in packages) {
                PkgFile pkg = PkgFile.Open(path);
                pkgs.Add(pkg);
                log(Path.GetFileName(path) + ": " + Describe(pkg));
            }
            if (pkgs.Count == 0) throw new PkgException("no packages given");
            string id = pkgs[0].TitleId;
            foreach (PkgFile pkg in pkgs) {
                if (pkg.TitleId != id) throw new PkgException("the packages are for different games (" + id + ", " + pkg.TitleId + ")");
                if (pkg.Category != "gd" && pkg.Category != "gp") throw new PkgException(Path.GetFileName(pkg.Path) + " is not a game or update package");
                if (pkg.DeltaPatch) throw new PkgException(Path.GetFileName(pkg.Path) + " is a delta update, which cannot be installed over a folder");
            }
            // The game first, then the updates by version.
            pkgs.Sort((a, b) => a.Category != b.Category ? (a.Category == "gd" ? -1 : 1)
                                                         : string.CompareOrdinal(a.AppVersion, b.AppVersion));
            string dest = Path.Combine(gameRoot, id);
            foreach (PkgFile pkg in pkgs) {
                if (pkg.Category == "gd") {
                    if (File.Exists(Path.Combine(dest, "eboot.bin"))) {
                        log("The game is already installed in " + dest + "; skipping " + Path.GetFileName(pkg.Path));
                        continue;
                    }
                    if (Directory.Exists(dest) && Directory.EnumerateFileSystemEntries(dest).Any())
                        throw new PkgException(dest + " exists and is not a game install; choose another folder or remove it");
                    string partial = dest + ".partial";
                    if (Directory.Exists(partial)) Directory.Delete(partial, true);
                    log("Installing the game to " + dest);
                    Extract(pkg, partial, log, progress);
                    if (Directory.Exists(dest)) Directory.Delete(dest);
                    Directory.Move(partial, dest);
                } else {
                    if (!File.Exists(Path.Combine(dest, "eboot.bin")))
                        throw new PkgException("install the game package before its update (no game in " + dest + ")");
                    string partial = dest + ".update-partial";
                    if (Directory.Exists(partial)) Directory.Delete(partial, true);
                    log("Installing update v" + pkg.AppVersion + " into " + dest);
                    Extract(pkg, partial, log, progress);
                    Merge(partial, dest);
                    Directory.Delete(partial, true);
                }
            }
            return dest;
        } finally {
            foreach (PkgFile pkg in pkgs) pkg.Dispose();
        }
    }

    /// Moves every file of source over the same path in dest.
    static void Merge(string source, string dest) {
        foreach (string dir in Directory.GetDirectories(source, "*", SearchOption.AllDirectories))
            Directory.CreateDirectory(Path.Combine(dest, dir.Substring(source.Length + 1)));
        foreach (string file in Directory.GetFiles(source, "*", SearchOption.AllDirectories)) {
            string target = Path.Combine(dest, file.Substring(source.Length + 1));
            if (File.Exists(target)) File.Delete(target);
            File.Move(file, target);
        }
    }
}

}
