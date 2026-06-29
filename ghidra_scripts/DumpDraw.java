// Decompile the terminal text-draw helpers to understand why only rows that
// previously held content get painted (persistent-surface / dirty-region theory).
//   FUN_005f4a90 - draws a run of glyphs
//   FUN_005d84f0 - measures a run width
//   FUN_00514ea0 - background/underline fill
//   FUN_00422d30 - the engine's own per-cell writer (what marks rows "live"?)
// Output: notes/draw.txt

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

import java.io.PrintWriter;

public class DumpDraw extends GhidraScript {
    @Override
    public void run() throws Exception {
        PrintWriter pw = new PrintWriter("C:\\Ghidra\\ghidra_projects\\notes\\draw.txt", "UTF-8");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);

        long[] addrs = { 0x005f4a90L, 0x005d84f0L, 0x00514ea0L, 0x00422d30L };
        for (long a : addrs) {
            Function f = getFunctionContaining(toAddr(Long.toHexString(a)));
            pw.println("==================================================");
            if (f == null) { pw.println("no function at " + Long.toHexString(a)); continue; }
            pw.println("FUNCTION " + f.getName() + " @ " + f.getEntryPoint());
            DecompileResults res = dec.decompileFunction(f, 60, monitor);
            if (res != null && res.decompileCompleted())
                pw.println(res.getDecompiledFunction().getC());
            else
                pw.println("(decompile failed)");
            pw.println();
        }
        dec.dispose();
        pw.close();
        println("DumpDraw: wrote notes/draw.txt");
    }
}
