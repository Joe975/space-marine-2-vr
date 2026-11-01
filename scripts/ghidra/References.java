// Read-only references to native functions/data. Args: output file, address, ...
import java.io.FileWriter;
import java.io.PrintWriter;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class References extends GhidraScript {
    public void run() throws Exception {
        String[] args=getScriptArgs();
        try(PrintWriter output=new PrintWriter(new FileWriter(args[0]))) {
            for(int i=1;i<args.length;i++) {
                var address=toAddr(args[i]);output.println("=== "+address);
                for(Reference reference:getReferencesTo(address)) {
                    var from=reference.getFromAddress();Function function=getFunctionContaining(from);
                    output.println(from+" "+reference.getReferenceType()+" "+(function==null?"data":function.getEntryPoint()+" size="+function.getBody().getNumAddresses()));
                }
            }
        }
    }
}
