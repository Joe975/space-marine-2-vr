// Lists every reference to each given ASCII string, with the containing function.
// Args: <output file> <file with one string per line>
// For references that come from data (name tables), prints the address so the table can be inspected.
import java.io.FileWriter;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;

public class StringXrefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Memory mem = currentProgram.getMemory();
        try (PrintWriter out = new PrintWriter(new FileWriter(args[0]))) {
            for (String s : Files.readAllLines(Paths.get(args[1]), StandardCharsets.UTF_8)) {
                if (s.isEmpty()) {
                    continue;
                }
                out.println("=== " + s);
                byte[] pattern = (s + "\0").getBytes(StandardCharsets.US_ASCII);
                Address a = mem.findBytes(currentProgram.getMinAddress(), pattern, null, true, monitor);
                while (a != null) {
                    boolean atStart;
                    try {
                        atStart = mem.getByte(a.subtract(1)) == 0;
                    } catch (Exception e) {
                        atStart = true;
                    }
                    if (atStart) {
                        out.println("  at " + a);
                        for (Reference r : getReferencesTo(a)) {
                            Address from = r.getFromAddress();
                            Function f = getFunctionContaining(from);
                            MemoryBlock block = mem.getBlock(from);
                            out.println("    ref " + from + " " + r.getReferenceType() + " "
                                + (f != null
                                    ? f.getName() + " @ " + f.getEntryPoint() + " size=" + f.getBody().getNumAddresses()
                                    : "(data in " + (block != null ? block.getName() : "?") + ")"));
                        }
                    }
                    a = mem.findBytes(a.add(1), pattern, null, true, monitor);
                }
            }
        }
    }
}
