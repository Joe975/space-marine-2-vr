// Read-only instruction range, including starts that fall inside an instruction.
// Args: output file, start address, exclusive end address (at most 64 KiB).
import java.io.FileWriter;
import java.io.PrintWriter;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;

public class InstructionRange extends GhidraScript {
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Address start = toAddr(args[1]), end = toAddr(args[2]);
        long length = end.subtract(start);
        if (length <= 0 || length > 65536) throw new IllegalArgumentException("Range outside 1..65536 bytes");
        try (PrintWriter out = new PrintWriter(new FileWriter(args[0]))) {
            Instruction instruction = getInstructionContaining(start);
            if (instruction == null) instruction = getInstructionAfter(start);
            while (instruction != null && instruction.getAddress().compareTo(end) < 0) {
                monitor.checkCancelled();
                out.print(instruction.getAddress() + " ");
                for (byte value : instruction.getBytes()) out.printf("%02x", value & 255);
                out.println(" " + instruction);
                instruction = instruction.getNext();
            }
        }
    }
}
