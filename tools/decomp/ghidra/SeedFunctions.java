// Seed exact EH frame entries before analysis. Game data stays in the private project.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.PointerDataType;
import com.google.gson.*;
import java.nio.file.*;
import java.util.*;

public class SeedFunctions extends GhidraScript {
    public void run() throws Exception {
        JsonObject seeds = JsonParser.parseString(Files.readString(Path.of(getScriptArgs()[0]))).getAsJsonObject();
        if (currentProgram.getImageBase().getOffset() != 0)
            throw new IllegalStateException("Image must be based at zero");
        println("BBPORT compiler=" + currentProgram.getCompilerSpec().getCompilerSpecID());
        Map<Long, String> guessedNames = loadGuessed(seeds);
        int done = 0, failed = 0;
        for (JsonElement e : seeds.getAsJsonArray("starts")) {
            long entry = Long.decode(e.getAsString());
            Address a = toAddr(entry);
            disassemble(a);
            if (getFunctionAt(a) == null && createFunction(a, null) == null) {
                failed++; println("BBPORT seed failed " + a);
            }
            String guessed = guessedNames.get(entry);
            if (guessed != null) {
                Symbol sym = getSymbolAt(a);
                if (sym != null) sym.setName(guessed, SourceType.USER_DEFINED);
                else createLabel(a, guessed, true, SourceType.USER_DEFINED);
            }
            if (++done % 10000 == 0) println("BBPORT seeded " + done + " failed=" + failed);
        }
        for (JsonElement e : seeds.getAsJsonArray("imports")) {
            JsonObject s = e.getAsJsonObject();
            Address a = toAddr(Long.decode(s.get("address").getAsString()));
            String name = s.get("name").getAsString().replaceAll("[^A-Za-z0-9_]", "_");
            createLabel(a, "imp_" + name, true, SourceType.USER_DEFINED);
            setPlateComment(a, "Orbis import " + s.get("nid").getAsString());
            if (getDataAt(a) == null) createData(a, new PointerDataType());
        }
        println("BBPORT seed total=" + done + " failed=" + failed + " guessed=" + guessedNames.size());
        if (failed != 0) throw new IllegalStateException("Some FDE functions were not created");
    }

    private static Map<Long, String> loadGuessed(JsonObject seeds) {
        Map<Long, String> map = new HashMap<>();
        JsonObject guessed = seeds.getAsJsonObject("guessed_names");
        if (guessed == null) return map;
        for (Map.Entry<String, JsonElement> e : guessed.entrySet())
            map.put(Long.decode(e.getKey()), e.getValue().getAsString());
        return map;
    }
}
