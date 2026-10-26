// Apply analyst names, then decompile a target list to C pseudocode.
//
// RAPT_OUT     output dir (default <user.dir>/out)
// RAPT_NAMES   addr<TAB>name map applied as function names (default notes/names.tsv)
// RAPT_TARGETS one hex function address per line (default notes/targets.txt)
//
// Writes out/pseudo/<addr>_<name>.c plus out/pseudo/manifest.tsv
// (addr, name, size, strings referenced by the function body).
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.SourceType;
import java.io.File;
import java.io.PrintWriter;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

public class AnnotateDecompile extends GhidraScript {

    private String env(String k, String d) {
        String v = System.getenv(k);
        return (v == null || v.isEmpty()) ? d : v;
    }

    public void run() throws Exception {
        String root = System.getProperty("user.dir");
        String out = env("RAPT_OUT", root + "/out");
        String names = env("RAPT_NAMES", root + "/notes/names.tsv");
        String targets = env("RAPT_TARGETS", root + "/notes/targets.txt");
        new File(out + "/pseudo").mkdirs();

        // 1. names
        Map<String, String> nameMap = new HashMap<>();
        File nf = new File(names);
        if (nf.exists()) {
            for (String line : Files.readAllLines(nf.toPath())) {
                String[] p = line.split("\t");
                if (p.length >= 2 && p[0].matches("[0-9a-fA-F]{4,16}")) nameMap.put(p[0].toLowerCase(), p[1].trim());
            }
        }
        int named = 0;
        int tx = currentProgram.startTransaction("analyst names");
        try {
            for (Map.Entry<String, String> e : nameMap.entrySet()) {
                Address a = toAddr("0x" + e.getKey());
                Function f = currentProgram.getFunctionManager().getFunctionAt(a);
                if (f == null) { println("no function at " + e.getKey()); continue; }
                if (!e.getValue().equals(f.getName())) {
                    f.setName(e.getValue(), SourceType.USER_DEFINED);
                    named++;
                }
            }
        } finally {
            currentProgram.endTransaction(tx, true);
        }

        // 2. decompile targets
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        int n = 0, fail = 0;
        try (PrintWriter man = new PrintWriter(out + "/pseudo/manifest.tsv")) {
            man.println("addr\tname\tcfile\tstrings");
            for (String line : Files.readAllLines(Paths.get(targets))) {
                String a = line.trim();
                if (a.isEmpty() || a.startsWith("#") || !a.matches("[0-9a-fA-F]{4,16}")) continue;
                Function f = currentProgram.getFunctionManager().getFunctionAt(toAddr("0x" + a.toLowerCase()));
                if (f == null) { println("no function at " + a); continue; }
                DecompileResults r = di.decompileFunction(f, 120, monitor);
                if (r == null || !r.decompileCompleted()) {
                    println("decompile failed " + a + " " + (r == null ? "null" : r.getErrorMessage()));
                    fail++;
                    continue;
                }
                String c = r.getDecompiledFunction().getC();
                String fn = String.format("%s_%s.c", a.toLowerCase(), f.getName().replaceAll("[^A-Za-z0-9_.-]", "_"));
                try (PrintWriter pw = new PrintWriter(out + "/pseudo/" + fn)) { pw.print(c); }

                // strings referenced from the body
                Set<String> strs = new LinkedHashSet<>();
                InstructionIterator it = currentProgram.getListing()
                    .getInstructions(f.getBody(), true);
                while (it.hasNext() && strs.size() < 40) {
                    Instruction ins = it.next();
                    for (Reference ref : currentProgram.getReferenceManager().getReferencesFrom(ins.getAddress())) {
                        ghidra.program.model.listing.Data dd =
                            currentProgram.getListing().getDefinedDataAt(ref.getToAddress());
                        Object v = dd == null ? null : dd.getValue();
                        if (v instanceof String) {
                            String s = ((String) v).replace("\t", " ").replace("\n", "\\n");
                            strs.add(String.format("%08x:%s", ref.getToAddress().getOffset(),
                                s.length() > 70 ? s.substring(0, 70) + "..." : s));
                        }
                    }
                }
                man.printf("%s\t%s\t%s\t%s%n", a.toLowerCase(), f.getName(), fn, String.join(" | ", strs));
                n++;
            }
        }
        printf("DECOMPILE DONE named=%d targets=%d failures=%d -> %s/pseudo%n", named, n, fail, out);
    }
}
