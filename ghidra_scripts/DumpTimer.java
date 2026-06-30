// Decompile the typewriter-reveal timer (FUN_0062ef60) to confirm its unit.
// Output: notes/timer.txt
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;

public class DumpTimer extends GhidraScript {
    @Override
    public void run() throws Exception {
        PrintWriter pw = new PrintWriter("C:\\Ghidra\\ghidra_projects\\notes\\timer.txt", "UTF-8");
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        long[] addrs = { 0x0062ef60L };
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
        println("DumpTimer: wrote notes/timer.txt");
    }
}
