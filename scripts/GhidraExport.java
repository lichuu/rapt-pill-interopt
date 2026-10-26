// Export block / function / call-graph / string inventories from the analyzed program.
// Env RAPT_OUT = output directory (default: <user.dir>/out)
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import java.io.File;
import java.io.PrintWriter;

public class GhidraExport extends GhidraScript {

    public void run() throws Exception {
        String out = System.getenv("RAPT_OUT");
        if (out == null) out = System.getProperty("user.dir") + "/out";
        new File(out).mkdirs();
        final ghidra.program.model.listing.Listing listing = currentProgram.getListing();
        final ghidra.program.model.mem.Memory mem = currentProgram.getMemory();
        final ghidra.program.model.listing.FunctionManager fm = currentProgram.getFunctionManager();
        final ghidra.program.model.symbol.ReferenceManager rm = currentProgram.getReferenceManager();

        try (PrintWriter pw = new PrintWriter(out + "/blocks.tsv")) {
            pw.println("name\tstart\tend\texec\twrite\tread\tinit\tsize");
            for (MemoryBlock b : mem.getBlocks()) {
                pw.printf("%s\t%08x\t%08x\t%b\t%b\t%b\t%b\t%x%n", b.getName(),
                    b.getStart().getOffset(), b.getEnd().getOffset(), b.isExecute(),
                    b.isWrite(), b.isRead(), b.isInitialized(), b.getSize());
            }
        }

        int instr = 0;
        InstructionIterator ii = listing.getInstructions(true);
        while (ii.hasNext()) { ii.next(); instr++; }

        try (PrintWriter pw = new PrintWriter(out + "/functions.tsv")) {
            pw.println("addr\tname\tnrefs");
            for (Function f : fm.getFunctions(true)) {
                if (monitor.isCancelled()) return;
                int n = rm.getReferenceCountTo(f.getEntryPoint());
                pw.printf("%08x\t%s\t%d%n", f.getEntryPoint().getOffset(), f.getName(), n);
            }
        }

        try (PrintWriter pw = new PrintWriter(out + "/calls.tsv")) {
            pw.println("from\tto");
            for (Function f : fm.getFunctions(true)) {
                if (monitor.isCancelled()) return;
                for (Function c : f.getCalledFunctions(monitor)) {
                    pw.printf("%08x\t%08x%n", f.getEntryPoint().getOffset(),
                        c.getEntryPoint().getOffset());
                }
            }
        }

        // Every decoded instruction's data references: addr -> targets (literal pools,
        // string loads). Big file, so keep it compact.
        try (PrintWriter pw = new PrintWriter(out + "/refs.tsv")) {
            pw.println("from\tto");
            InstructionIterator it = listing.getInstructions(true);
            while (it.hasNext()) {
                if (monitor.isCancelled()) return;
                Instruction ins = it.next();
                for (Reference r : rm.getReferencesFrom(ins.getAddress())) {
                    pw.printf("%08x\t%08x%n", ins.getAddress().getOffset(),
                        r.getToAddress().getOffset());
                }
            }
        }

        // strings.tsv: address, first referencing function, text
        try (PrintWriter pw = new PrintWriter(out + "/strings.tsv")) {
            pw.println("addr\tfunc\tstring");
            DataIterator di = listing.getDefinedData(true);
            while (di.hasNext()) {
                if (monitor.isCancelled()) return;
                Data d = di.next();
                if (!(d.getValue() instanceof String)) continue;
                String s = (String) d.getValue();
                if (s.length() < 4) continue;
                String fn = "";
                for (Reference r : rm.getReferencesTo(d.getAddress())) {
                    Function f = fm.getFunctionContaining(r.getFromAddress());
                    if (f != null) { fn = String.format("%08x", f.getEntryPoint().getOffset()); break; }
                }
                s = s.replace("\t", " ").replace("\n", "\\n").replace("\r", "");
                if (s.length() > 160) s = s.substring(0, 160) + "...";
                pw.printf("%08x\t%s\t%s%n", d.getAddress().getOffset(), fn, s);
            }
        }

        int funcs = fm.getFunctionCount();
        printf("EXPORT DONE instructions=%d functions=%d out=%s%n", instr, funcs, out);
        try (PrintWriter pw = new PrintWriter(out + "/summary.txt")) {
            pw.printf("instructions=%d%nfunctions=%d%n", instr, funcs);
        }
    }
}
