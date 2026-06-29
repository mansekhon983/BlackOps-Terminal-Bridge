// Dump the renderer's caller (FUN_004cbf70) and a couple of hops up, to find
// whether/how the per-frame terminal renderer FUN_00575be0 is gated (only called
// on terminal activity vs. every frame). Output: notes/caller.txt
//
// We also decompile FUN_00575be0's caller chain so we can see the condition that
// guards the call to the terminal renderer.

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;

import java.io.PrintWriter;
import java.util.*;

public class DumpCaller extends GhidraScript {

    @Override
    public void run() throws Exception {
        PrintWriter pw = new PrintWriter("C:\\Ghidra\\ghidra_projects\\notes\\caller.txt", "UTF-8");
        ReferenceManager rm = currentProgram.getReferenceManager();
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);

        // Walk up from the terminal renderer to find the per-frame gate.
        long[] targets = { 0x004cbf70L, 0x00575be0L };
        Set<Long> seen = new HashSet<>();
        Deque<Long> work = new ArrayDeque<>();
        for (long t : targets) work.add(t);

        int budget = 8;   // decompile at most this many functions up the chain
        while (!work.isEmpty() && budget-- > 0) {
            long addr = work.poll();
            if (!seen.add(addr)) { budget++; continue; }
            Function f = getFunctionContaining(toAddr(Long.toHexString(addr)));
            if (f == null) { pw.println("no function at " + Long.toHexString(addr)); continue; }

            pw.println("==================================================");
            pw.println("FUNCTION " + f.getName() + " @ " + f.getEntryPoint());
            pw.println("callers:");
            for (Reference r : rm.getReferencesTo(f.getEntryPoint())) {
                Function caller = getFunctionContaining(r.getFromAddress());
                pw.println("   <- " + r.getFromAddress() + "  " + (caller==null?"?":caller.getName()));
                if (caller != null) work.add(caller.getEntryPoint().getOffset());
            }
            DecompileResults res = dec.decompileFunction(f, 60, monitor);
            if (res != null && res.decompileCompleted())
                pw.println(res.getDecompiledFunction().getC());
            else
                pw.println("(decompile failed)");
            pw.println();
        }
        dec.dispose();
        pw.close();
        println("DumpCaller: wrote notes/caller.txt");
    }
}
