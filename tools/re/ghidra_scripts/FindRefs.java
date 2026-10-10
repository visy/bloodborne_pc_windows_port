// Finds references: for each argument, either a hex address (references to it) or a text
// pattern (defined strings containing it, ASCII or UTF-16, and the functions referencing them).
//   analyzeHeadless <proj> bloodborne -process eboot.elf -noanalysis -readOnly
//     -scriptPath tools/re/ghidra_scripts -postScript FindRefs.java <out.txt> <addr|text>...
// @category bbport
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.FileWriter;
import java.io.PrintWriter;

public class FindRefs extends GhidraScript {
    private void refsTo(PrintWriter out, Address a, String indent) {
        int n = 0;
        for (Reference r : getReferencesTo(a)) {
            Function f = getFunctionContaining(r.getFromAddress());
            out.printf("%s%s %s in %s%n", indent, r.getFromAddress(), r.getReferenceType(),
                    f == null ? "?" : f.getName() + "@" + f.getEntryPoint());
            if (++n >= 200) {
                out.printf("%s...%n", indent);
                break;
            }
        }
        if (n == 0) {
            out.printf("%s(no references)%n", indent);
        }
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        try (PrintWriter out = new PrintWriter(new FileWriter(args[0]))) {
            for (int i = 1; i < args.length; ++i) {
                String q = args[i];
                out.printf("==== %s%n", q);
                if (q.matches("(?i)0x[0-9a-f]+")) {
                    refsTo(out, currentProgram.getAddressFactory().getDefaultAddressSpace()
                            .getAddress(Long.parseUnsignedLong(q.substring(2), 16)), "  ");
                    continue;
                }
                int found = 0;
                DataIterator it = currentProgram.getListing().getDefinedData(true);
                while (it.hasNext() && !monitor.isCancelled()) {
                    Data d = it.next();
                    if (!d.hasStringValue()) {
                        continue;
                    }
                    Object v = d.getValue();
                    if (v == null || !v.toString().contains(q)) {
                        continue;
                    }
                    out.printf("  string %s \"%s\"%n", d.getAddress(), v.toString().replace("\n", "\\n"));
                    refsTo(out, d.getAddress(), "    ");
                    if (++found >= 50) {
                        out.println("  ...");
                        break;
                    }
                }
                if (found == 0) {
                    out.println("  (no defined string contains this text)");
                }
            }
        }
    }
}
