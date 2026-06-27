// Decompile key terminal functions + dump disassembly windows around the
// help-screen string XREFs (to expose the line_append call target).
// Output: notes/decomp.txt

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Listing;

import java.io.PrintWriter;

public class DumpFuncs extends GhidraScript {

    // Functions to fully decompile (entry points or any address inside them).
    private final String[] DECOMP = {
        "0x0055f800",   // dispatcher / unknown-command handler
        "0x005545e0",   // login handler (PASSWORD:, Invalid Password)
        "0x005aa2a0",   // loads systems.csv / users.csv (fake FS)
        "0x00420240",   // detachFromTerminal (lifecycle)
        "0x00551ced",   // help-screen printer (no defined function -> window fallback)
    };

    // Addresses to dump a raw instruction window around (call-target visibility).
    private final String[] WINDOWS = {
        "0x00551c00",   // "System Help:" push site -> start of help printer
    };

    @Override
    public void run() throws Exception {
        String out = "C:\\Ghidra\\ghidra_projects\\notes\\decomp.txt";
        PrintWriter pw = new PrintWriter(out, "UTF-8");
        pw.println("# Decompilation + disassembly of key terminal functions");
        pw.println("# imageBase: " + currentProgram.getImageBase());
        pw.println();

        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Listing listing = currentProgram.getListing();

        for (String a : DECOMP) {
            Address addr = toAddr(a);
            Function fn = getFunctionContaining(addr);
            pw.println("======================================================");
            if (fn == null) {
                pw.println("NO FUNCTION containing " + addr + " -> disassembly window");
                dumpWindow(pw, listing, addr, 8, 60);
                pw.println();
                continue;
            }
            pw.println("FUNCTION " + fn.getName() + " @ " + fn.getEntryPoint()
                       + "  (from probe " + addr + ")");
            pw.println("------ decompiled ------");
            DecompileResults r = dec.decompileFunction(fn, 60, monitor);
            if (r != null && r.decompileCompleted()) {
                pw.println(r.getDecompiledFunction().getC());
            } else {
                pw.println("(decompile failed: "
                    + (r == null ? "null" : r.getErrorMessage()) + ")");
            }
            pw.println();
        }

        for (String a : WINDOWS) {
            Address addr = toAddr(a);
            pw.println("======================================================");
            pw.println("WINDOW around " + addr);
            dumpWindow(pw, listing, addr, 4, 80);
            pw.println();
        }

        dec.dispose();
        pw.close();
        println("DumpFuncs: wrote " + out);
    }

    // Print `count` instructions starting `back` instructions before addr,
    // annotating CALL targets.
    private void dumpWindow(PrintWriter pw, Listing listing, Address addr,
                            int back, int count) {
        Instruction ins = listing.getInstructionContaining(addr);
        if (ins == null) { pw.println("  (no instruction at " + addr + ")"); return; }
        for (int i = 0; i < back && ins != null; i++) {
            Instruction prev = ins.getPrevious();
            if (prev == null) break;
            ins = prev;
        }
        for (int i = 0; i < count && ins != null; i++) {
            String mn = ins.getMnemonicString();
            StringBuilder sb = new StringBuilder();
            sb.append("  ").append(ins.getAddress()).append("  ").append(ins.toString());
            if (mn.toUpperCase().startsWith("CALL") || mn.toUpperCase().startsWith("J")) {
                Address[] flows = ins.getFlows();
                if (flows != null && flows.length > 0)
                    sb.append("    -> ").append(flows[0]);
            }
            pw.println(sb.toString());
            ins = ins.getNext();
        }
    }
}
