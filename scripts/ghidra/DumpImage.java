// Writes the program's initialized memory, laid out by RVA (a mapped PE image), to a file.
// Used to recover the bytes of an older game build from its Ghidra project after Steam has
// replaced the binaries on disk. Usage: -postScript DumpImage.java <out file>
//@category sm2vr
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.MemoryBlock;
import java.io.RandomAccessFile;

public class DumpImage extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) { printerr("usage: DumpImage.java <out>"); return; }
        Address base = currentProgram.getImageBase();
        try (RandomAccessFile out = new RandomAccessFile(args[0], "rw")) {
            out.setLength(0);
            for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
                if (!block.isInitialized() || block.isOverlay()) continue;
                long rva = block.getStart().subtract(base);
                if (rva < 0) continue;
                byte[] bytes = new byte[(int) block.getSize()];
                int read = block.getBytes(block.getStart(), bytes);
                out.seek(rva);
                out.write(bytes, 0, read);
                println(String.format("block %s rva=0x%x size=0x%x", block.getName(), rva, read));
            }
        }
        println("image base " + base + " written to " + args[0]);
    }
}
