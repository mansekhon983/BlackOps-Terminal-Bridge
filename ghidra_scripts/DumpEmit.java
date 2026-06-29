// Decompile the actual glyph emitter + line-height helper to find any
// scissor/clip that would drop sparse rows.
//   FUN_006d65c0 - the glyph emitter (clip/scissor lives here if anywhere)
//   FUN_004dfa30 - line-height
// Also list every function that WRITES the grid char buffer DAT_00bf56a0,
// to catch a per-frame rebuild that could overwrite our blit.
// Output: notes/emit.txt

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;

import java.io.PrintWriter;

public class DumpEmit extends GhidraScript {
    @Override
    public void run() throws Exception {
        PrintWriter pw = new PrintWriter("C:\\Ghidra\\ghidra_projects\\notes\\emit.txt", "UTF-8");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);

        long[] addrs = { 0x006d65c0L, 0x004dfa30L };
        for (long a : addrs) {
            Function f = getFunctionContaining(toAddr(Long.toHexString(a)));
            pw.println("==================================================");
            if (f == null) { pw.println("no function at " + Long.toHexString(a)); continue; }
            pw.println("FUNCTION " + f.getName() + " @ " + f.getEntryPoint());
            DecompileResults res = dec.decompileFunction(f, 90, monitor);
            pw.println(res != null && res.decompileCompleted()
                ? res.getDecompiledFunction().getC() : "(decompile failed)");
            pw.println();
        }

        // Who WRITES the grid char buffer 0x00bf56a0? A per-frame rebuild here
        // would clobber our emuBlit before the draw loop reads it.
        pw.println("================ writers of DAT_00bf56a0 (grid chars) ===========");
        Address grid = toAddr("00bf56a0");
        ReferenceManager rm = currentProgram.getReferenceManager();
        for (Reference ref : rm.getReferencesTo(grid)) {
            Address from = ref.getFromAddress();
            Function f = getFunctionContaining(from);
            pw.println(from + "  " + ref.getReferenceType() + "  in " +
                       (f != null ? f.getName() + " @ " + f.getEntryPoint() : "(none)"));
        }
        dec.dispose();
        pw.close();
        println("DumpEmit: wrote notes/emit.txt");
    }
}
