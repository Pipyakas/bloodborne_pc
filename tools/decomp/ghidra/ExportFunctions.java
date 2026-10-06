// Read-only batch pseudo-C and instruction export, with a per-function index.
// Each invocation appends to <out>/index.csv (skipping addresses already
// present), so a full export can be resumed across chunks after an
// interruption. Game data stays in the private output directory.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.nio.file.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import com.google.gson.*;

public class ExportFunctions extends GhidraScript {
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Path out = Path.of(args[0]); Files.createDirectories(out);
        Path index = out.resolve("index.csv");
        // Optional second argument: a file with one hex address per line,
        // used instead of trailing command-line addresses so a batch of
        // 10,000 functions does not exceed the OS command-line limit.
        List<String> requested = new ArrayList<>();
        int firstArg = 1;
        if (args.length > 1 && args[1].startsWith("@")) {
            Path list = Path.of(args[1].substring(1));
            for (String line : Files.readAllLines(list, StandardCharsets.UTF_8)) {
                String s = line.strip();
                if (!s.isEmpty()) requested.add(s);
            }
            firstArg = 2;
        }
        for (int i = firstArg; i < args.length; i++) requested.add(args[i]);
        Set<String> done = new HashSet<>();
        if (Files.exists(index)) {
            for (String line : Files.readAllLines(index, StandardCharsets.UTF_8)) {
                int comma = line.indexOf(',');
                if (comma > 0) done.add(line.substring(0, comma));
            }
        }
        List<String> records = new ArrayList<>();
        DecompInterface decompiler = new DecompInterface();
        try {
            if (!decompiler.openProgram(currentProgram)) throw new IllegalStateException(decompiler.getLastMessage());
            JsonArray metrics = new JsonArray();
            int skipped = 0;
            for (int i=0; i<requested.size(); i++) {
                long address = Long.parseUnsignedLong(requested.get(i).replaceFirst("^(0x|0X)", ""), 16);
                String stem = Long.toHexString(address);
                if (done.contains(stem)) { skipped++; continue; }
                Function f = getFunctionAt(toAddr(address));
                if (f == null) {
                    records.add(stem + ",0,,absent,false");
                    println("BBPORT export " + stem + " status=absent");
                    continue;
                }
                String name = f.getName();
                long size = f.getBody().getNumAddresses();
                String c = null;
                String error = "";
                long start = System.nanoTime();
                try {
                    DecompileResults result = decompiler.decompileFunction(f, 60, monitor);
                    if (result.decompileCompleted()) c = result.getDecompiledFunction().getC();
                    else error = result.getErrorMessage();
                } catch (Exception e) { error = String.valueOf(e); }
                double seconds = (System.nanoTime()-start)/1e9;
                if (c != null) {
                    String header = "/* Ghidra pseudo-C; not verified source. entry=0x" + stem +
                        " compiler=" + currentProgram.getCompilerSpec().getCompilerSpecID() + " */\n";
                    Files.writeString(out.resolve(stem+".c"), header+c, StandardCharsets.UTF_8);
                    records.add(stem + "," + size + "," + name.replace(',', ';') + ",ok,true");
                } else {
                    String safeError = error.replace('\n', ' ').replace('\r', ' ')
                        .replace(',', ';').replace('"', '\'');
                    records.add(stem + "," + size + "," + name.replace(',', ';')
                        + ",failed:" + safeError + ",false");
                }
                StringBuilder asm = new StringBuilder("; entry=0x"+stem+"\n");
                InstructionIterator instructions = currentProgram.getListing().getInstructions(f.getBody(), true);
                while (instructions.hasNext()) {
                    Instruction ins = instructions.next();
                    asm.append(ins.getAddress()).append("  ");
                    for (byte b : ins.getBytes()) asm.append(String.format("%02x", b & 255)).append(' ');
                    asm.append("  ").append(ins).append('\n');
                }
                Files.writeString(out.resolve(stem+".asm"), asm.toString(), StandardCharsets.UTF_8);
                JsonObject metric = new JsonObject(); metric.addProperty("address", stem);
                metric.addProperty("seconds", seconds); metrics.add(metric);
                println("BBPORT export " + stem + " seconds=" + seconds + (c != null ? "" : " decompile_failed"));
            }
            if (!records.isEmpty()) {
                boolean fresh = !Files.exists(index);
                if (fresh)
                    Files.writeString(index, "address,size,name,decompile,decompiled\n",
                        StandardCharsets.UTF_8, StandardOpenOption.CREATE);
                Files.write(index, records, StandardCharsets.UTF_8,
                    StandardOpenOption.CREATE, StandardOpenOption.APPEND);
            }
            Files.writeString(out.resolve("export-metrics.json"), metrics.toString());
            println("BBPORT export batch requested=" + requested.size() + " skipped_already_done=" + skipped
                + " written=" + metrics.size());
        } finally { decompiler.dispose(); }
    }
}
