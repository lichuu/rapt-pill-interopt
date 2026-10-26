// Xtensa function discovery, encoding-agnostic.
//
// Ghidra's code discovery follows resolvable calls; on Xtensa most calls are
// register-indirect (call4/call8 rs), so whole modules stay undiscovered.
// A function start is any executable address that something points at:
//   - the target of any reference (code xrefs, literal-pool loads)
//   - any word in a non-executable, initialized block whose value is an
//     executable address (handler tables, task entry points, ISR vectors)
// Each candidate that is not already inside a function gets disassembled and,
// if it decodes cleanly, turned into a function. No opcode pattern matching,
// so it is not sensitive to the density/16-bit subset of the core.
//
// RAPT_MAX cap (default 20000)
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.ReferenceManager;
import java.util.ArrayList;
import java.util.List;
import java.util.TreeSet;

public class DiscoverFunctions extends GhidraScript {

    public void run() throws Exception {
        int cap = 20000;
        try { cap = Integer.parseInt(System.getenv().getOrDefault("RAPT_MAX", "20000")); } catch (Exception e) {}
        final Listing listing = currentProgram.getListing();
        final Memory mem = currentProgram.getMemory();
        final FunctionManager fm = currentProgram.getFunctionManager();
        final ReferenceManager rm = currentProgram.getReferenceManager();

        // executable address ranges
        final List<long[]> code = new ArrayList<>();
        for (MemoryBlock b : mem.getBlocks()) {
            if (b.isExecute() && b.isInitialized())
                code.add(new long[]{b.getStart().getOffset(), b.getEnd().getOffset()});
        }

        // data words that name code addresses (handler/task/ISR tables)
        TreeSet<Long> cand = new TreeSet<>();
        for (MemoryBlock b : mem.getBlocks()) {
            if (!b.isInitialized() || b.isExecute()) continue;
            int len = (int) Math.min(b.getSize(), 8L * 1024 * 1024);
            byte[] buf = new byte[len];
            int got = mem.getBytes(b.getStart(), buf, 0, len);
            for (int o = 0; o + 4 <= got; o += 4) {
                long v = (buf[o] & 0xffL) | ((buf[o + 1] & 0xffL) << 8)
                    | ((buf[o + 2] & 0xffL) << 16) | ((buf[o + 3] & 0xffL) << 24);
                for (long[] r : code) if (r[0] <= v && v <= r[1]) { cand.add(v); break; }
            }
        }
        int fromData = cand.size();

        // plus every reference target that is executable
        ReferenceIterator ri = rm.getReferenceIterator(currentProgram.getMinAddress());
        while (ri.hasNext()) {
            Address to = ri.next().getToAddress();
            for (long[] r : code)
                if (r[0] <= to.getOffset() && to.getOffset() <= r[1]) { cand.add(to.getOffset()); break; }
            if ((cand.size() & 0xfff) == 0 && monitor.isCancelled()) return;
        }
        int total = cand.size();

        int made = 0, refused = 0;
        int tx = currentProgram.startTransaction("referenced-address functions");
        try {
            for (Long v : cand) {
                if (made >= cap || monitor.isCancelled()) break;
                Address a = toAddr(v);
                if (fm.getFunctionAt(a) != null) continue;
                Function enclosing = fm.getFunctionContaining(a);
                if (enclosing != null && !enclosing.isThunk()) continue;
                if (listing.getInstructionAt(a) == null) {
                    AddressSet s = new AddressSet(a);
                    if (!new DisassembleCommand(s, null).applyTo(currentProgram, monitor)) { refused++; continue; }
                }
                if (listing.getInstructionAt(a) == null) { refused++; continue; }
                if (new CreateFunctionCmd(a).applyTo(currentProgram, monitor)) made++;
                else refused++;
            }
        } finally {
            currentProgram.endTransaction(tx, true);
        }
        println("DISCOVER dataPointers=" + fromData + " candidates=" + total
            + " created=" + made + " refused=" + refused + " functions=" + fm.getFunctionCount());
    }
}
