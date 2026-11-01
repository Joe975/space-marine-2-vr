import java.io.FileWriter;
import java.io.PrintWriter;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
public class ValueBytes extends GhidraScript {
    public void run() throws Exception {
        String[] args=getScriptArgs();
        try(PrintWriter out=new PrintWriter(new FileWriter(args[0]))) {
            for(int j=1;j<args.length;j++) {
                Address at=toAddr(args[j]);byte[] bytes=getBytes(at,64);
                out.printf("%s float=%g bytes=",at,Float.intBitsToFloat(getInt(at)));
                for(byte value:bytes) out.printf("%02x",value&255);
                out.print(" ascii=");for(byte value:bytes) out.print(value>=32&&value<127?(char)value:'.');out.println();
            }
        }
    }
}
