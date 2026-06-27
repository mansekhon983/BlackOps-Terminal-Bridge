// Headless recon: find terminal-relevant defined strings and their XREFs.
// Run via analyzeHeadless against the existing Black_Ops project (no re-analysis).
// Output: notes/terminal_strings.txt  (address | string | xref -> containing function)

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

import java.io.PrintWriter;
import java.util.Locale;

public class DumpTerminalStrings extends GhidraScript {

    // Keywords that hint a string belongs to the terminal easter egg.
    private static final String[] KEYS = {
        "zork", "west of house", "doa", "dead ops", "arcade", "rlogin",
        "arpanet", "alicia", "amber", "foobar", "3arc", "datacenter",
        "press enter", "login", "password", "incorrect", "help",
        "command not", "syntax", "directory", "you are likely to be eaten",
        "mainframe", "terminal", "access granted", "access denied"
    };

    @Override
    public void run() throws Exception {
        String out = "C:\\Ghidra\\ghidra_projects\\notes\\terminal_strings.txt";
        PrintWriter pw = new PrintWriter(out, "UTF-8");
        pw.println("# Terminal-relevant defined strings + XREFs");
        pw.println("# program: " + currentProgram.getName());
        pw.println("# imageBase: " + currentProgram.getImageBase());
        pw.println();

        int hits = 0;
        DataIterator it = currentProgram.getListing().getDefinedData(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Data d = it.next();
            if (d == null || !d.hasStringValue()) continue;
            Object v = d.getValue();
            if (v == null) continue;
            String s = v.toString();
            String low = s.toLowerCase(Locale.ROOT);

            boolean match = false;
            for (String k : KEYS) { if (low.contains(k)) { match = true; break; } }
            if (!match) continue;

            Address addr = d.getAddress();
            String oneLine = s.replace("\n", "\\n").replace("\r", "\\r");
            if (oneLine.length() > 120) oneLine = oneLine.substring(0, 120) + "...";

            pw.println("STRING @ " + addr + "  \"" + oneLine + "\"");
            Reference[] refs = getReferencesTo(addr);
            if (refs.length == 0) {
                pw.println("    (no xrefs)");
            }
            for (Reference r : refs) {
                Address from = r.getFromAddress();
                Function fn = getFunctionContaining(from);
                String fname = (fn == null) ? "?" : (fn.getName() + " @ " + fn.getEntryPoint());
                pw.println("    XREF from " + from + "  in  " + fname);
            }
            pw.println();
            hits++;
        }
        pw.println("# total matching strings: " + hits);
        pw.close();
        println("DumpTerminalStrings: wrote " + hits + " strings to " + out);
    }
}
