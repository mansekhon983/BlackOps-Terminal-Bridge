// Decompile line_append + helpers and emit entry-point byte signatures
// (for runtime sigscan instead of hardcoded addresses).
// Output: notes/sigs.txt

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

import java.io.PrintWriter;

public class DumpSig extends GhidraScript {

    // name -> entry address
    private final String[][] FUNCS = {
        {"line_append",    "0x004ef440"},
        {"line_commit",    "0x00433720"},
        {"dispatch_submit","0x0055f800"},
        {"get_input_token","0x00566b30"},
        {"cbuf_exec",      "0x0049b930"},
    };

    private final int SIG_LEN = 24;

    @Override
    public void run() throws Exception {
        String out = "C:\\Ghidra\\ghidra_projects\\notes\\sigs.txt";
        PrintWriter pw = new PrintWriter(out, "UTF-8");
        pw.println("# Entry-point signatures + decompiled contracts");
        pw.println("# imageBase: " + currentProgram.getImageBase());
        pw.println();

        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);

        for (String[] f : FUNCS) {
            String name = f[0];
            Address addr = toAddr(f[1]);
            pw.println("==== " + name + " @ " + addr + " (RVA 0x"
                + Long.toHexString(addr.getOffset() - 0x400000) + ") ====");

            // Raw entry bytes -> IDA-style pattern.
            byte[] bytes = new byte[SIG_LEN];
            StringBuilder hex = new StringBuilder();
            for (int i = 0; i < SIG_LEN; i++) {
                int b = getByte(addr.add(i)) & 0xff;
                if (i > 0) hex.append(' ');
                String h = Integer.toHexString(b).toUpperCase();
                if (h.length() == 1) h = "0" + h;
                hex.append(h);
            }
            pw.println("bytes: " + hex);
            pw.println();
        }

        // Decompile line_append and the input tokenizer for the contract.
        for (String entry : new String[]{"0x004ef440", "0x00566b30"}) {
            Function fn = getFunctionContaining(toAddr(entry));
            pw.println("------ decompile " + entry + " ------");
            if (fn != null) {
                DecompileResults r = dec.decompileFunction(fn, 60, monitor);
                if (r != null && r.decompileCompleted())
                    pw.println(r.getDecompiledFunction().getC());
                else
                    pw.println("(decompile failed)");
            } else {
                pw.println("(no function)");
            }
            pw.println();
        }

        dec.dispose();
        pw.close();
        println("DumpSig: wrote " + out);
    }
}
