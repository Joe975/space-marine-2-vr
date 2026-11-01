import java.io.FileWriter;
import java.io.PrintWriter;
import ghidra.app.script.GhidraScript;
public class FunctionRange extends GhidraScript {
 public void run() throws Exception {
  String[] a=getScriptArgs();var end=toAddr(a[2]);
  try(PrintWriter out=new PrintWriter(new FileWriter(a[0]))){
   var iter=currentProgram.getFunctionManager().getFunctions(toAddr(a[1]),true);
   while(iter.hasNext()){var f=iter.next();if(f.getEntryPoint().compareTo(end)>=0)break;out.println(f.getEntryPoint());}
  }
 }
}
