// One-pass text export of the analysed program for fast grepping (no Ghidra needed afterwards).
// Writes into <out_dir>: functions.tsv, strings.tsv, xrefs_to_strings.tsv, calls.tsv.
//   analyzeHeadless <proj_dir> bloodborne -process eboot.elf -noanalysis -readOnly
//     -scriptPath tools/re/ghidra_scripts -postScript ExportAll.java <out_dir>
// Used by tools/agent/ghidra_decomp.py --export. Addresses are our guest offsets (hex, 0x...).
// @category bbport
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressIterator;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;
import java.io.BufferedWriter;
import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;

public class ExportAll extends GhidraScript {
    private PrintWriter open(File dir, String name, String header) throws Exception {
        PrintWriter w = new PrintWriter(new BufferedWriter(new OutputStreamWriter(
                new FileOutputStream(new File(dir, name)), StandardCharsets.UTF_8), 1 << 20));
        w.println(header);
        return w;
    }

    private static String hex(Address a) { return "0x" + Long.toHexString(a.getOffset()); }

    private static String clean(String s) {
        StringBuilder b = new StringBuilder(s.length());
        for (char c : s.toCharArray()) {
            if (c == '\t') b.append("\\t");
            else if (c == '\n') b.append("\\n");
            else if (c == '\r') b.append("\\r");
            else if (c < 0x20) b.append(String.format("\\x%02x", (int) c));
            else b.append(c);
        }
        return b.toString();
    }

    private static String fname(Function f) { return f == null ? "?" : f.getName(); }
    private static String fentry(Function f) { return f == null ? "?" : hex(f.getEntryPoint()); }

    @Override
    public void run() throws Exception {
        File dir = new File(getScriptArgs()[0]);
        dir.mkdirs();
        FunctionManager fm = currentProgram.getFunctionManager();
        ReferenceManager rm = currentProgram.getReferenceManager();

        long n = 0;
        try (PrintWriter w = open(dir, "functions.tsv", "#entry\tname\tsize\tbody_end")) {
            FunctionIterator it = fm.getFunctions(true);
            while (it.hasNext()) {
                Function f = it.next();
                w.printf("%s\t%s\t%d\t%s%n", hex(f.getEntryPoint()), f.getName(),
                        f.getBody().getNumAddresses(), hex(f.getBody().getMaxAddress()));
                ++n;
            }
        }
        println("functions: " + n);

        n = 0;
        long x = 0;
        try (PrintWriter ws = open(dir, "strings.tsv", "#address\ttype\tstring");
             PrintWriter wx = open(dir, "xrefs_to_strings.tsv",
                     "#string_addr\tfrom_addr\tfunc_entry\tfunc_name\tstring")) {
            DataIterator it = currentProgram.getListing().getDefinedData(true);
            while (it.hasNext()) {
                Data d = it.next();
                if (!d.hasStringValue()) continue;
                Object v = d.getValue();
                if (v == null) continue;
                String s = clean(v.toString());
                ws.printf("%s\t%s\t%s%n", hex(d.getAddress()), d.getDataType().getName(), s);
                ++n;
                String sh = s.length() > 120 ? s.substring(0, 120) : s;
                for (Reference r : getReferencesTo(d.getAddress())) {
                    Function f = fm.getFunctionContaining(r.getFromAddress());
                    wx.printf("%s\t%s\t%s\t%s\t%s%n", hex(d.getAddress()), hex(r.getFromAddress()),
                            fentry(f), fname(f), sh);
                    ++x;
                }
            }
        }
        println("strings: " + n + " string xrefs: " + x);

        n = 0;
        try (PrintWriter w = open(dir, "calls.tsv",
                "#caller_entry\tcaller_name\tcall_site\tcallee_entry\tcallee_name\tkind")) {
            AddressIterator src = rm.getReferenceSourceIterator(currentProgram.getMinAddress(), true);
            while (src.hasNext()) {
                if (monitor.isCancelled()) break;
                Address from = src.next();
                for (Reference r : rm.getReferencesFrom(from)) {
                    if (!r.getReferenceType().isCall()) continue;
                    Function caller = fm.getFunctionContaining(from);
                    Function callee = fm.getFunctionAt(r.getToAddress());
                    w.printf("%s\t%s\t%s\t%s\t%s\t%s%n", fentry(caller), fname(caller), hex(from),
                            hex(r.getToAddress()), fname(callee), r.getReferenceType().getName());
                    ++n;
                }
            }
        }
        println("calls: " + n);
    }
}
