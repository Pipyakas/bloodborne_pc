// Read-only quality counters and examples; private output, not a correctness proof.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.nio.file.*;
import com.google.gson.*;

public class AuditQuality extends GhidraScript {
    public void run() throws Exception {
        JsonObject seeds = JsonParser.parseString(Files.readString(Path.of(getScriptArgs()[0]))).getAsJsonObject();
        JsonObject audit = new JsonObject();
        audit.addProperty("compiler", currentProgram.getCompilerSpec().getCompilerSpecID().toString());
        audit.addProperty("default_calling_convention", currentProgram.getCompilerSpec().getDefaultCallingConvention().getName());
        audit.addProperty("image_base", currentProgram.getImageBase().toString());
        int missing=0, strings=0, switches=0;
        JsonArray missingAddresses = new JsonArray();
        for (JsonElement e : seeds.getAsJsonArray("starts")) {
            if (getFunctionAt(toAddr(Long.decode(e.getAsString()))) == null) {
                missing++;
                missingAddresses.add(e.getAsString());
            }
        }
        audit.addProperty("fde_entries_missing", missing);
        audit.add("fde_entries_missing_addresses", missingAddresses);
        audit.addProperty("total_functions", currentProgram.getFunctionManager().getFunctionCount());
        JsonArray stringExamples = new JsonArray(), switchExamples = new JsonArray();
        DataIterator data = currentProgram.getListing().getDefinedData(true);
        while (data.hasNext()) {
            monitor.checkCancelled();
            Data d = data.next();
            if (d.hasStringValue()) {
                strings++;
                if (stringExamples.size()<10) {
                    JsonObject example = new JsonObject(); example.addProperty("address", d.getAddress().toString());
                    example.addProperty("value", String.valueOf(d.getValue())); stringExamples.add(example);
                }
            }
        }
        SymbolIterator symbols = currentProgram.getSymbolTable().getAllSymbols(true);
        while (symbols.hasNext()) {
            Symbol s = symbols.next();
            if (s.getName().startsWith("switchD")) {
                switches++;
                if (switchExamples.size()<10) {
                    Function f = getFunctionContaining(s.getAddress());
                    JsonObject example = new JsonObject(); example.addProperty("address", s.getAddress().toString());
                    example.addProperty("name", s.getName());
                    if (f != null) example.addProperty("function", f.getEntryPoint().toString());
                    switchExamples.add(example);
                }
            }
        }
        audit.addProperty("defined_strings", strings); audit.addProperty("switch_symbols", switches);
        audit.add("string_examples", stringExamples); audit.add("switch_examples", switchExamples);
        // Name-application audit: counts of seeded labels.
        int importLabels=0, guessedLabels=0, appliedGuesses=0;
        JsonObject guessedSeeds = seeds.getAsJsonObject("guessed_names");
        java.util.Set<Long> expected = new java.util.HashSet<>();
        if (guessedSeeds != null)
            for (String key : guessedSeeds.keySet())
                expected.add(Long.decode(key));
        java.util.Set<Long> importAddresses = new java.util.HashSet<>();
        for (JsonElement e : seeds.getAsJsonArray("imports"))
            importAddresses.add(Long.decode(e.getAsJsonObject()
                .get("address").getAsString()));
        java.util.Set<Long> seenImportAddresses = new java.util.HashSet<>();
        SymbolIterator allSymbols = currentProgram.getSymbolTable().getAllSymbols(true);
        while (allSymbols.hasNext()) {
            Symbol s = allSymbols.next();
            String n = s.getName();
            if (n.startsWith("imp_")) {
                importLabels++;
                seenImportAddresses.add(s.getAddress().getOffset());
            }
            if (n.startsWith("guess_")) {
                guessedLabels++;
                if (expected.contains(s.getAddress().getOffset())) appliedGuesses++;
            }
        }
        audit.addProperty("import_labels", importLabels);
        audit.addProperty("guess_labels", guessedLabels);
        audit.addProperty("guess_labels_at_seeded_addresses", appliedGuesses);
        audit.addProperty("guessed_names_expected", expected.size());
        audit.addProperty("import_slots_expected", seeds.getAsJsonArray("imports").size());
        audit.addProperty("import_addresses_covered", seenImportAddresses.size());
        audit.addProperty("import_addresses_expected", importAddresses.size());
        Files.writeString(Path.of(getScriptArgs()[1]), audit.toString());
        println("BBPORT audit " + audit);
        if (importLabels != seeds.getAsJsonArray("imports").size())
            throw new IllegalStateException("Import slot labels missing at some addresses");
        if (appliedGuesses != expected.size())
            throw new IllegalStateException("Guessed name labels missing");
    }
}
