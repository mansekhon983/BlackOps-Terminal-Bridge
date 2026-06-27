// Dump entry-point bytes for the terminal frame renderer (for RVA+verify hook).
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import java.io.PrintWriter;

public class GetBytes extends GhidraScript {
    @Override public void run() throws Exception {
        PrintWriter pw = new PrintWriter("C:\\Ghidra\\ghidra_projects\\notes\\getbytes.txt", "UTF-8");
        String[] addrs = { "0x00575be0" };  // frame renderer FUN_00575be0
        for (String a : addrs) {
            Address addr = toAddr(a);
            StringBuilder hex = new StringBuilder();
            for (int i = 0; i < 28; i++) {
                int b = getByte(addr.add(i)) & 0xff;
                if (i > 0) hex.append(' ');
                String h = Integer.toHexString(b).toUpperCase();
                if (h.length() == 1) h = "0" + h;
                hex.append(h);
            }
            pw.println(a + " (RVA 0x" + Long.toHexString(addr.getOffset()-0x400000) + "): " + hex);
        }
        pw.close();
        println("GetBytes: done");
    }
}
