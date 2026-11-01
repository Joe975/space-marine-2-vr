// Dump native instructions and bytes for known addresses, without saving analysis changes.
// Args: output file, address, instruction count, ...
import java.io.FileWriter;
import java.io.PrintWriter;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;

public class Instructions extends GhidraScript {
    public void run() throws Exception {
        String[] args=getScriptArgs();
        try (PrintWriter out=new PrintWriter(new FileWriter(args[0]))) {
            for(int i=1;i<args.length;i+=2) {
                Address address=toAddr(args[i]);
                int count=Integer.parseInt(args[i+1]);
                if(count<1 || count>1000) throw new IllegalArgumentException("Instruction count outside 1..1000");
                Instruction instruction=getInstructionAt(address);
                if(instruction==null) { disassemble(address); instruction=getInstructionAt(address); }
                out.println("=== "+address);
                for(int n=0;n<count && instruction!=null;n++,instruction=instruction.getNext()) {
                    out.print(instruction.getAddress()+" ");
                    for(byte value:instruction.getBytes()) out.printf("%02x",value & 255);
                    out.println(" "+instruction);
                }
            }
        }
    }
}
