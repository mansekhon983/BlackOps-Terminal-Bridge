// E3 recon: how do keystrokes reach the terminal key handler FUN_0047eb90, and
// how is the typed character delivered? Decides the input-hook approach (engine
// key-handler hook vs WndProc subclass) and the keycode->VT table.
//   - decompile FUN_0047eb90 (key handler) + its callers, 2 levels up
//   - decompile FUN_0057ff80 (dvar getter used for ui_keyboard_focus_key)
//   - scan for WndProc / Windows-message input (WM_KEYDOWN 0x100, WM_CHAR 0x102,
//     GetMessage/PeekMessage/DispatchMessage/DefWindowProc) vs DirectInput
//   - find keycode anchors: strings naming arrow/nav keys near the handler
// Output: notes/input-recon.txt
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;

public class ReconInput extends GhidraScript {
    DecompInterface dec;
    PrintWriter pw;

    String decompile(Function f) {
        if (f == null) return "(null)";
        DecompileResults r = dec.decompileFunction(f, 60, monitor);
        if (r != null && r.decompileCompleted()) return r.getDecompiledFunction().getC();
        return "(decompile failed)";
    }

    void dumpFn(String label, Address a) {
        Function f = getFunctionContaining(a);
        pw.println("================= " + label + " @ " + a + " (" + (f==null?"?":f.getName()) + ") =================");
        if (f == null) { pw.println("(no function)"); return; }
        pw.println("callers:");
        for (Reference r : currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint())) {
            Function c = getFunctionContaining(r.getFromAddress());
            pw.println("   <- " + r.getFromAddress() + "  " + (c==null?"?":c.getName()));
        }
        pw.println(decompile(f));
        pw.println();
    }

    @Override
    public void run() throws Exception {
        pw = new PrintWriter("C:\\Ghidra\\ghidra_projects\\notes\\input-recon.txt", "UTF-8");
        dec = new DecompInterface();
        dec.openProgram(currentProgram);

        // 1. The key handler and the dvar getter.
        dumpFn("key_handler FUN_0047eb90", toAddr("0047eb90"));
        dumpFn("dvar_getter FUN_0057ff80", toAddr("0057ff80"));

        // 2. Its callers (engine key dispatch) and THEIR callers, to find the source.
        Set<Address> level1 = new LinkedHashSet<>();
        for (Reference r : currentProgram.getReferenceManager().getReferencesTo(toAddr("0047eb90"))) {
            Function c = getFunctionContaining(r.getFromAddress());
            if (c != null) level1.add(c.getEntryPoint());
        }
        for (Address a : level1) dumpFn("caller(L1)", a);

        Set<Address> level2 = new LinkedHashSet<>();
        for (Address a1 : level1) {
            Function f1 = getFunctionAt(a1);
            if (f1 == null) continue;
            for (Reference r : currentProgram.getReferenceManager().getReferencesTo(f1.getEntryPoint())) {
                Function c = getFunctionContaining(r.getFromAddress());
                if (c != null) level2.add(c.getEntryPoint());
            }
        }
        for (Address a : level2) dumpFn("caller(L2)", a);

        // 3. Input-source indicators: import names + key strings.
        pw.println("########## input-source indicators ##########");
        String[] needles = {
            "dinput","DirectInput","GetDeviceData","GetDeviceState","IDirectInputDevice",
            "GetMessage","PeekMessage","DispatchMessage","DefWindowProc","WndProc",
            "RegisterClass","TranslateMessage",
            "ui_keyboard_focus_key","uin_text_type","keycatch","cl_bypassMouse",
            "UPARROW","DOWNARROW","LEFTARROW","RIGHTARROW","HOME","END","PGUP","PGDN",
            "BACKSPACE","ENTER","SPACE","ESCAPE","TAB","DEL","KP_"
        };
        Listing lst = currentProgram.getListing();
        ReferenceManager rm = currentProgram.getReferenceManager();
        DataIterator di = lst.getDefinedData(true);
        int hits = 0;
        while (di.hasNext() && hits < 400) {
            Data d = di.next();
            Object v = d.getValue();
            if (!(v instanceof String)) continue;
            String s = (String) v;
            for (String n : needles) {
                if (s.toLowerCase().contains(n.toLowerCase())) {
                    StringBuilder refs = new StringBuilder();
                    for (Reference r : rm.getReferencesTo(d.getAddress())) {
                        Function c = getFunctionContaining(r.getFromAddress());
                        refs.append(" <").append(c==null?"?":c.getName()).append('@').append(r.getFromAddress()).append('>');
                    }
                    pw.println(d.getAddress() + "  \"" + s + "\"" + refs);
                    hits++;
                    break;
                }
            }
        }

        // 4. Imported functions from dinput8.dll / user32 message APIs.
        pw.println("\n########## imports (dinput / user32 input) ##########");
        SymbolTable st = currentProgram.getSymbolTable();
        SymbolIterator it = st.getExternalSymbols();
        while (it.hasNext()) {
            Symbol s = it.next();
            String nm = s.getName();
            String low = nm.toLowerCase();
            if (low.contains("directinput") || low.contains("getdevice") ||
                nm.equals("GetMessageA") || nm.equals("PeekMessageA") ||
                nm.equals("DispatchMessageA") || nm.equals("DefWindowProcA") ||
                nm.equals("TranslateMessage") || nm.equals("RegisterClassA") ||
                nm.equals("RegisterClassExA") || nm.equals("CallWindowProcA") ||
                nm.equals("SetWindowLongA") || nm.equals("SetWindowLongPtrA")) {
                StringBuilder refs = new StringBuilder();
                for (Reference r : rm.getReferencesTo(s.getAddress())) {
                    Function c = getFunctionContaining(r.getFromAddress());
                    refs.append(" <").append(c==null?"?":c.getName()).append('@').append(r.getFromAddress()).append('>');
                }
                pw.println(nm + "  @" + s.getAddress() + refs);
            }
        }

        dec.dispose();
        pw.close();
        println("ReconInput: wrote notes/input-recon.txt");
    }
}
