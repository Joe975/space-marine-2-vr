import java.io.FileWriter;
import java.io.PrintWriter;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
public class PointerTable extends GhidraScript {
    public void run() throws Exception {
        String[] args=getScriptArgs();
        try(PrintWriter out=new PrintWriter(new FileWriter(args[0]))) {
            for(int j=1;j<args.length;j++) {
                Address start=toAddr(args[j]);
                for(int i=0;i<40;i++) {
                    Address at=start.add(i*8);long value=getLong(at);Address target=toAddr(value);
                    Function f=getFunctionAt(target);
                    out.printf("%s +%03x %016x %s%n",at,i*8,value,f==null?"":f.getName());
                }
            }
        }
    }
}
