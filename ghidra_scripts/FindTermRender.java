// Find the terminal's per-frame update/render function so we can drip shell
// output one-frame-at-a-time (instead of dumping it all synchronously).
// Strategy: the renderer touches the cell/row buffer DAT_00bf5f10 and the line
// counter DAT_00bf8120 heavily. List referencing functions by ref-count and
// decompile the top candidates.
// Output: notes/render.txt

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;

import java.io.PrintWriter;
import java.util.*;

public class FindTermRender extends GhidraScript {

    @Override
    public void run() throws Exception {
        PrintWriter pw = new PrintWriter("C:\\Ghidra\\ghidra_projects\\notes\\render.txt", "UTF-8");
        ReferenceManager rm = currentProgram.getReferenceManager();

        long[] globals = { 0x00bf5f10L, 0x00bf8120L };
        Map<Function,int[]> counts = new HashMap<>(); // [refs to 5f10, refs to 8120]

        for (int gi = 0; gi < globals.length; gi++) {
            Address g = toAddr(Long.toHexString(globals[gi]));
            for (Reference r : rm.getReferencesTo(g)) {
                Function f = getFunctionContaining(r.getFromAddress());
                if (f == null) continue;
                counts.computeIfAbsent(f, k -> new int[2])[gi]++;
            }
        }

        List<Function> fns = new ArrayList<>(counts.keySet());
        fns.sort((a,b) -> {
            int[] ca = counts.get(a), cb = counts.get(b);
            return (cb[0]+cb[1]) - (ca[0]+ca[1]);
        });

        pw.println("# Functions referencing terminal buffer 0xBF5F10 / counter 0xBF8120");
        pw.println("# (entry  name  refs5f10  refs8120  bodySize)");
        for (Function f : fns) {
            int[] c = counts.get(f);
            long size = f.getBody().getNumAddresses();
            pw.println(String.format("%s  %-22s  5f10=%-3d 8120=%-3d  size=%d",
                f.getEntryPoint(), f.getName(), c[0], c[1], size));
        }
        pw.println();

        // Decompile the top 3 candidates.
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        int n = 0;
        for (Function f : fns) {
            if (n++ >= 3) break;
            pw.println("==================================================");
            pw.println("FUNCTION " + f.getName() + " @ " + f.getEntryPoint()
                + "  refs5f10=" + counts.get(f)[0] + " refs8120=" + counts.get(f)[1]);
            // Who calls it? (helps tell per-frame from on-demand.)
            pw.println("callers:");
            for (Reference r : rm.getReferencesTo(f.getEntryPoint())) {
                Function caller = getFunctionContaining(r.getFromAddress());
                pw.println("   <- " + r.getFromAddress() + "  " + (caller==null?"?":caller.getName()));
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
        println("FindTermRender: wrote notes/render.txt");
    }
}
