// SPDX-License-Identifier: GPL-2.0-or-later
// bbport-pkg.exe (setup.bat builds it next to bbport-setup.exe): PS4 package installation from a
// command line, with PkgInstall.cs.
//   bbport-pkg info <pkg>...
//   bbport-pkg install <games folder> <game pkg> [update pkg...]   -> <games folder>\<TITLE_ID>
//   bbport-pkg extract <pkg> <output folder>
using System;
using System.Diagnostics;
using System.Linq;

namespace BbportSetup {

static class PkgTool {
    static int Main(string[] args) {
        try {
            if (args.Length >= 2 && args[0] == "info") {
                foreach (string path in args.Skip(1)) {
                    using (PkgFile pkg = PkgFile.Open(path)) {
                        Console.WriteLine("{0}: {1}", path, PkgInstaller.Describe(pkg));
                        Console.WriteLine("  content {0}, flags 0x{1:X8}, {2} entries, PFS {3:F1} GB at 0x{4:X}", pkg.ContentId,
                                          pkg.ContentFlags, pkg.Entries.Count, pkg.PfsSize / 1e9, pkg.PfsOffset);
                    }
                }
                return 0;
            }
            if (args.Length >= 3 && args[0] == "install") {
                string dest = PkgInstaller.Install(args.Skip(2).ToList(), args[1], Console.WriteLine, Reporter());
                Console.WriteLine();
                Console.WriteLine("Installed: " + dest);
                return 0;
            }
            if (args.Length == 3 && args[0] == "extract") {
                using (PkgFile pkg = PkgFile.Open(args[1])) {
                    Console.WriteLine(PkgInstaller.Describe(pkg));
                    PkgInstaller.Extract(pkg, args[2], Console.WriteLine, Reporter());
                }
                Console.WriteLine();
                Console.WriteLine("Extracted to " + args[2]);
                return 0;
            }
            Console.Error.WriteLine("usage: bbport-pkg info <pkg>... | install <games folder> <game pkg> [update pkg...] | extract <pkg> <output folder>");
            return 2;
        } catch (Exception e) {
            Console.WriteLine();
            Console.Error.WriteLine("error: " + e.Message);
            return 1;
        }
    }

    /// A progress line about once a second.
    static PkgInstaller.Progress Reporter() {
        var clock = Stopwatch.StartNew();
        long last = -1000, previous = 0;
        object gate = new object();
        return (done, total, file) => {
            lock (gate) {
                if (done < previous) { clock.Restart(); last = -1000; } // the next package
                previous = done;
                long now = clock.ElapsedMilliseconds;
                if (now - last < 1000 && done < total) return;
                last = now;
                double rate = done / 1e6 / Math.Max(0.001, now / 1000.0);
                Console.WriteLine("{0,5:F1}% {1,6:F1} of {2:F1} GB, {3:F0} MB/s", 100.0 * done / Math.Max(1, total),
                                  done / 1e9, total / 1e9, rate);
            }
        };
    }
}

}
