// Recon for the full-terminal-emulator feasibility study. Answers:
//  (1) the display-grid cell writer + cursor model (FUN_00422d30 and the
//      grid globals DAT_00bf56a0 chars / DAT_00bf44d8 attrs / DAT_00bf812c cursor)
//  (2) per-cell colour: who writes the attribute grid, with what values
//  (3) the KEYBOARD path: which functions WRITE the input buffer DAT_00bf8130
//      (keystroke capture), and their callers (trace toward the WndProc/Key_Event).
// Output: notes/emu-recon.txt
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;

public class ReconTermEmu extends GhidraScript {

    DecompInterface dec;

    String decompile(Function f) {
        if (f == null) return "(null fn)";
        DecompileResults res = dec.decompileFunction(f, 60, monitor);
        if (res != null && res.decompileCompleted())
            return res.getDecompiledFunction().getC();
        return "(decompile failed)";
    }

    void dumpRefs(PrintWriter pw, long addr, String label) {
        ReferenceManager rm = currentProgram.getReferenceManager();
        Address g = toAddr(Long.toHexString(addr));
        pw.println("---- refs to " + label + " (" + Long.toHexString(addr) + ") ----");
        Set<Function> readers = new LinkedHashSet<>();
        Set<Function> writers = new LinkedHashSet<>();
        for (Reference r : rm.getReferencesTo(g)) {
            Function f = getFunctionContaining(r.getFromAddress());
            String fn = (f == null ? "?" : f.getName() + "@" + f.getEntryPoint());
            RefType rt = r.getReferenceType();
            boolean w = rt.isWrite();
            pw.println(String.format("   %s  %-5s  in %s", r.getFromAddress(),
                (w ? "WRITE" : (rt.isRead() ? "read" : rt.toString())), fn));
            if (f != null) { if (w) writers.add(f); else readers.add(f); }
        }
        pw.println("   writers: " + writers.size() + "  readers: " + readers.size());
        pw.println();
    }

    @Override
    public void run() throws Exception {
        PrintWriter pw = new PrintWriter("C:\\Ghidra\\ghidra_projects\\notes\\emu-recon.txt", "UTF-8");
        dec = new DecompInterface();
        dec.openProgram(currentProgram);

        pw.println("########## (1)(2) DISPLAY GRID + CURSOR ##########\n");
        // Reference maps for the grid globals seen in FUN_00575be0's draw loop.
        dumpRefs(pw, 0x00bf56a0L, "grid_chars DAT_00bf56a0");
        dumpRefs(pw, 0x00bf44d8L, "grid_attrs DAT_00bf44d8");
        dumpRefs(pw, 0x00bf812cL, "cursor DAT_00bf812c");
        dumpRefs(pw, 0x00bf8234L, "active_shell_idx DAT_00bf8234");

        // The cell writer invoked by the reveal path (case 1) in the renderer.
        Function cw = getFunctionAt(toAddr("00422d30"));
        pw.println("===== FUN_00422d30 (display-grid cell writer) =====");
        pw.println("callers:");
        for (Reference r : currentProgram.getReferenceManager().getReferencesTo(toAddr("00422d30"))) {
            Function c = getFunctionContaining(r.getFromAddress());
            pw.println("   <- " + r.getFromAddress() + "  " + (c==null?"?":c.getName()));
        }
        pw.println(decompile(cw));
        pw.println();

        pw.println("########## (3) KEYBOARD / INPUT CAPTURE ##########\n");
        // The dispatcher reads the typed line from DAT_00bf8130; whoever WRITES it
        // is the per-keystroke capture (or the line assembler). Trace it up.
        dumpRefs(pw, 0x00bf8130L, "input_buffer DAT_00bf8130");

        // Decompile every function that writes the input buffer (the capture path).
        ReferenceManager rm = currentProgram.getReferenceManager();
        LinkedHashSet<Function> writers = new LinkedHashSet<>();
        for (Reference r : rm.getReferencesTo(toAddr("00bf8130"))) {
            if (r.getReferenceType().isWrite()) {
                Function f = getFunctionContaining(r.getFromAddress());
                if (f != null) writers.add(f);
            }
        }
        for (Function f : writers) {
            pw.println("===== INPUT-WRITER " + f.getName() + " @ " + f.getEntryPoint() + " =====");
            pw.println("callers:");
            for (Reference r : rm.getReferencesTo(f.getEntryPoint())) {
                Function c = getFunctionContaining(r.getFromAddress());
                pw.println("   <- " + r.getFromAddress() + "  " + (c==null?"?":c.getName()));
            }
            pw.println(decompile(f));
            pw.println();
        }

        // Look for the engine key-event entry points by string anchors.
        pw.println("########## key-event string anchors ##########");
        String[] needles = {"Key_Event","keycatch","cl_bypassMouseInput","Field_KeyDown",
                             "in_kbd","WM_CHAR","keyDown","Con_KeyEvent"};
        DataIterator di = currentProgram.getListing().getDefinedData(true);
        int hits = 0;
        while (di.hasNext() && hits < 200) {
            Data d = di.next();
            Object v = d.getValue();
            if (!(v instanceof String)) continue;
            String s = (String) v;
            for (String n : needles) {
                if (s.toLowerCase().contains(n.toLowerCase())) {
                    pw.println(d.getAddress() + "  \"" + s + "\"");
                    for (Reference r : rm.getReferencesTo(d.getAddress())) {
                        Function c = getFunctionContaining(r.getFromAddress());
                        pw.println("     <- " + r.getFromAddress() + "  " + (c==null?"?":c.getName()));
                    }
                    hits++;
                    break;
                }
            }
        }

        dec.dispose();
        pw.close();
        println("ReconTermEmu: wrote notes/emu-recon.txt");
    }
}
