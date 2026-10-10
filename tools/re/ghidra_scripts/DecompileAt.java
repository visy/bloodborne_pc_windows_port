// Decompiles the functions containing the given addresses (our guest offsets, hex) and writes C
// pseudocode, callers and callees to an output file. Headless use:
//   analyzeHeadless <proj_dir> bloodborne -process eboot.elf -noanalysis -readOnly
//     -scriptPath tools/re/ghidra_scripts -postScript DecompileAt.java <out.txt> <addr> [addr...]
// @category bbport
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.Set;

public class DecompileAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("usage: DecompileAt.java <out.txt> <hex addr>...");
            return;
        }
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(new FileWriter(args[0]))) {
            for (int i = 1; i < args.length; ++i) {
                String text = args[i].toLowerCase().replace("0x", "");
                Address addr = currentProgram.getAddressFactory().getDefaultAddressSpace()
                        .getAddress(Long.parseUnsignedLong(text, 16));
                Function f = getFunctionContaining(addr);
                out.printf("==== 0x%s%n", text);
                if (f == null) {
                    out.println("no function contains this address");
                    continue;
                }
                out.printf("function %s at %s (body %s)%n", f.getName(), f.getEntryPoint(),
                        f.getBody().getMinAddress() + ".." + f.getBody().getMaxAddress());
                out.println("callers:");
                for (Reference r : getReferencesTo(f.getEntryPoint())) {
                    Function c = getFunctionContaining(r.getFromAddress());
                    out.printf("  %s from %s%n", r.getFromAddress(), c == null ? "?" : c.getName());
                }
                Set<Function> callees = f.getCalledFunctions(monitor);
                out.println("callees:");
                for (Function c : callees) {
                    out.printf("  %s %s%n", c.getEntryPoint(), c.getName());
                }
                DecompileResults r = decomp.decompileFunction(f, 120, monitor);
                if (r != null && r.decompileCompleted()) {
                    out.println(r.getDecompiledFunction().getC());
                } else {
                    out.println("decompilation failed: " + (r == null ? "?" : r.getErrorMessage()));
                }
            }
        }
        decomp.dispose();
    }
}
