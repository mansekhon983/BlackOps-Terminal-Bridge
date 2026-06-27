// Diagnose the M0 freeze: (1) confirm how FUN_0055f800 (dispatch_submit) is
// CALLED -> reveals its calling convention; (2) verify each hook signature
// matches exactly once in the image.
// Output: notes/check.txt

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;

import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;

public class CheckDispatch extends GhidraScript {

    @Override
    public void run() throws Exception {
        String out = "C:\\Ghidra\\ghidra_projects\\notes\\check.txt";
        PrintWriter pw = new PrintWriter(out, "UTF-8");
        Listing listing = currentProgram.getListing();

        // ---- (1) Callers of dispatch_submit -> convention -------------------
        Address disp = toAddr("0x0055f800");
        pw.println("# Callers of dispatch_submit (FUN_0055f800)");
        pw.println("# Look for: are args PUSHed before CALL (cdecl/stdcall) or");
        pw.println("# set in ECX/EDX (fastcall/thiscall) ? How is the stack");
        pw.println("# cleaned after the CALL (ADD ESP,x = cdecl; none = stdcall)?");
        pw.println();

        ReferenceManager rm = currentProgram.getReferenceManager();
        for (Reference r : rm.getReferencesTo(disp)) {
            if (!r.getReferenceType().isCall() && !r.getReferenceType().isData()) {
                // also include flow refs
            }
            Address from = r.getFromAddress();
            Instruction ins = listing.getInstructionContaining(from);
            if (ins == null) continue;
            String mn = ins.getMnemonicString().toUpperCase();
            if (!mn.startsWith("CALL") && !mn.startsWith("J")) continue;
            Function f = getFunctionContaining(from);
            pw.println("CALL site " + from + " in " + (f==null?"?":f.getName()));
            // 10 instructions before the call + the call
            Instruction cur = ins;
            List<Instruction> back = new ArrayList<>();
            for (int i=0;i<10 && cur!=null;i++){ Instruction p=cur.getPrevious(); if(p==null)break; back.add(0,p); cur=p; }
            for (Instruction b : back) pw.println("    " + b.getAddress() + "  " + b);
            pw.println("  > " + ins.getAddress() + "  " + ins);
            Instruction after = ins.getNext();
            for (int i=0;i<3 && after!=null;i++){ pw.println("    " + after.getAddress() + "  " + after); after=after.getNext(); }
            pw.println();
        }

        // ---- (2) Signature uniqueness check ---------------------------------
        pw.println("# Signature match counts (expect exactly 1 each)");
        checkSig(pw, "line_append",
            "83 EC 08 8B 44 24 0C 55 56 33 F6 89 74 24 08 33 ED 8D 50 01 8A 08 40 84");
        checkSig(pw, "line_commit_wild",
            "8B 0D ?? ?? ?? ?? B8 1F 85 EB 51 F7 E9 C1 FA 05 8B C2 C1 E8 1F 03 D0 6B");
        checkSig(pw, "line_commit_literal",
            "8B 0D 20 81 BF 00 B8 1F 85 EB 51 F7 E9 C1 FA 05 8B C2 C1 E8 1F 03 D0 6B");
        checkSig(pw, "dispatch_submit",
            "83 EC 08 A1 ?? ?? ?? ?? 8D 04 80 8B 04 85 ?? ?? ?? ?? C7 04 24 ?? ?? ?? ??");

        pw.close();
        println("CheckDispatch: wrote " + out);
    }

    private void checkSig(PrintWriter pw, String name, String ida) throws Exception {
        byte[] bytes; boolean[] mask;
        String[] toks = ida.trim().split("\\s+");
        bytes = new byte[toks.length]; mask = new boolean[toks.length];
        for (int i=0;i<toks.length;i++){
            if (toks[i].startsWith("?")) { mask[i]=false; bytes[i]=0; }
            else { mask[i]=true; bytes[i]=(byte)Integer.parseInt(toks[i],16); }
        }
        Memory mem = currentProgram.getMemory();
        List<Address> hits = new ArrayList<>();
        for (MemoryBlock blk : mem.getBlocks()) {
            if (!blk.isExecute()) continue;
            Address a = blk.getStart();
            byte[] buf = new byte[(int)blk.getSize()];
            try { mem.getBytes(a, buf); } catch (Exception e) { continue; }
            for (int i=0;i+bytes.length<=buf.length;i++){
                boolean ok=true;
                for (int j=0;j<bytes.length;j++){ if(mask[j] && buf[i+j]!=bytes[j]){ok=false;break;} }
                if (ok) hits.add(a.add(i));
            }
        }
        pw.print(name + ": " + hits.size() + " match(es)");
        for (Address h: hits) pw.print("  " + h);
        pw.println();
    }
}
