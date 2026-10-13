// Batch decompile into a cache directory: one <out_dir>/0x<addr>.c per requested address (the
// function containing it), with a header (entry, body, callers, callees). Each file is written to
// a .tmp name and renamed, so a killed run never leaves half a file.
//   analyzeHeadless <proj_dir> bloodborne -process eboot.elf -noanalysis -readOnly
//     -scriptPath tools/re/ghidra_scripts -postScript DecompileToDir.java <out_dir> <addr>...
// Used by tools/agent/ghidra_decomp.py.
// @category bbport
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;

public class DecompileToDir extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File dir = new File(args[0]);
        dir.mkdirs();
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        for (int i = 1; i < args.length; ++i) {
            long off = Long.parseUnsignedLong(args[i].toLowerCase().replace("0x", ""), 16);
            String key = "0x" + Long.toHexString(off);
            Address addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(off);
            File tmp = new File(dir, key + ".c.tmp");
            try (PrintWriter out = new PrintWriter(new OutputStreamWriter(
                    new FileOutputStream(tmp), StandardCharsets.UTF_8))) {
                Function f = getFunctionContaining(addr);
                out.printf("// ==== %s%n", key);
                if (f == null) {
                    out.println("// no function contains this address");
                } else {
                    out.printf("// function %s at 0x%x (body 0x%x..0x%x)%n", f.getName(),
                            f.getEntryPoint().getOffset(), f.getBody().getMinAddress().getOffset(),
                            f.getBody().getMaxAddress().getOffset());
                    out.println("// callers:");
                    for (Reference r : getReferencesTo(f.getEntryPoint())) {
                        Function c = getFunctionContaining(r.getFromAddress());
                        out.printf("//   0x%x from %s%n", r.getFromAddress().getOffset(),
                                c == null ? "?" : c.getName());
                    }
                    out.println("// callees:");
                    for (Function c : f.getCalledFunctions(monitor)) {
                        out.printf("//   0x%x %s%n", c.getEntryPoint().getOffset(), c.getName());
                    }
                    DecompileResults r = decomp.decompileFunction(f, 120, monitor);
                    if (r != null && r.decompileCompleted()) {
                        out.println(r.getDecompiledFunction().getC());
                    } else {
                        out.println("// decompilation failed: " + (r == null ? "?" : r.getErrorMessage()));
                    }
                }
            }
            File dst = new File(dir, key + ".c");
            dst.delete();
            tmp.renameTo(dst);
            println("decompiled " + key);
        }
        decomp.dispose();
    }
}
