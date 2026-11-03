// Finds instructions with a scalar operand equal to one of the given decimal values and prints the
// function that holds each (an FMG text id, a param id ...).
//   analyzeHeadless <proj> bloodborne -process eboot.elf -noanalysis -readOnly
//     -scriptPath tools/re/ghidra_scripts -postScript FindScalar.java <out.txt> <value>...
// @category bbport
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.scalar.Scalar;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.HashSet;
import java.util.Set;

public class FindScalar extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Set<Long> want = new HashSet<>();
        for (int i = 1; i < args.length; ++i) {
            want.add(Long.parseLong(args[i]));
        }
        try (PrintWriter out = new PrintWriter(new FileWriter(args[0]))) {
            InstructionIterator it = currentProgram.getListing().getInstructions(true);
            while (it.hasNext() && !monitor.isCancelled()) {
                Instruction ins = it.next();
                for (int op = 0; op < ins.getNumOperands(); ++op) {
                    Scalar s = ins.getScalar(op);
                    if (s != null && want.contains(s.getUnsignedValue())) {
                        Function f = getFunctionContaining(ins.getAddress());
                        out.printf("%d at %s: %s in %s%n", s.getUnsignedValue(), ins.getAddress(), ins,
                                f == null ? "?" : f.getName() + "@" + f.getEntryPoint());
                    }
                }
            }
        }
    }
}
