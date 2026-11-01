// Decompiles the functions containing the given addresses and lists who refers to them.
// Args: <output file> <address> [<address> ...]   (addresses in hex, image base 0x140000000)
// An address prefixed with "x:" only lists references to that address (for globals), no decompile.
import java.io.FileWriter;
import java.io.PrintWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class DecompileShading extends GhidraScript {
    private void listRefs(PrintWriter out, Address target) {
        int n = 0;
        for (Reference r : getReferencesTo(target)) {
            Function f = getFunctionContaining(r.getFromAddress());
            out.println("//   ref from " + r.getFromAddress() + " " + r.getReferenceType() + " "
                + (f != null ? f.getName() + " @ " + f.getEntryPoint() : "(data)"));
            if (++n >= 60) {
                out.println("//   ... more");
                break;
            }
        }
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(new FileWriter(args[0]))) {
            for (int i = 1; i < args.length; i++) {
                boolean refsOnly = args[i].startsWith("x:");
                Address a = toAddr(refsOnly ? args[i].substring(2) : args[i]);
                if (refsOnly) {
                    out.println("// ===== references to " + a);
                    listRefs(out, a);
                    continue;
                }
                Function f = getFunctionContaining(a);
                // Some small virtual methods are referenced only through tables and were
                // missed by the initial analysis. -readOnly discards these analysis changes.
                if (f == null && disassemble(a)) {
                    f = createFunction(a, null);
                }
                if (f == null) {
                    out.println("// ===== " + a + ": no function");
                    continue;
                }
                out.println("// ===== " + f.getName() + " @ " + f.getEntryPoint() + " size=" + f.getBody().getNumAddresses());
                listRefs(out, f.getEntryPoint());
                DecompileResults res = decomp.decompileFunction(f, 600, monitor);
                out.println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "// decompile failed: " + res.getErrorMessage());
            }
        } finally {
            decomp.dispose();
        }
    }
}

