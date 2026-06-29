// Decompile the glyph clip/emit path to find why blank/sparse rows are culled.
//   FUN_005cd7b0 - positions/clips a draw
//   FUN_0061c4d0 - emits the glyph geometry (scissor? whitespace cull?)
//   FUN_006d3110 - run measure helper
//   FUN_006d30f0 - font metric helper
// Output: notes/draw2.txt

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;

import java.io.PrintWriter;

public class DumpDraw2 extends GhidraScript {
    @Override
    public void run() throws Exception {
        PrintWriter pw = new PrintWriter("C:\\Ghidra\\ghidra_projects\\notes\\draw2.txt", "UTF-8");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        long[] addrs = { 0x005cd7b0L, 0x0061c4d0L, 0x006d3110L, 0x006d30f0L };
        for (long a : addrs) {
            Function f = getFunctionContaining(toAddr(Long.toHexString(a)));
            pw.println("==================================================");
            if (f == null) { pw.println("no function at " + Long.toHexString(a)); continue; }
            pw.println("FUNCTION " + f.getName() + " @ " + f.getEntryPoint());
            DecompileResults res = dec.decompileFunction(f, 60, monitor);
            pw.println(res != null && res.decompileCompleted()
                ? res.getDecompiledFunction().getC() : "(decompile failed)");
            pw.println();
        }
        dec.dispose();
        pw.close();
        println("DumpDraw2: wrote notes/draw2.txt");
    }
}
