// Headless Ghidra script: exports function list and decompiled pseudo-C for every function.
// Usage: analyzeHeadless ... -postScript ExportDecomp.java <outDir>
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

import java.io.File;
import java.io.PrintWriter;

public class ExportDecomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File outDir = new File(args.length > 0 ? args[0] : "decomp");
        outDir.mkdirs();

        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);

        try (PrintWriter list = new PrintWriter(new File(outDir, "functions.csv"), "UTF-8")) {
            list.println("entry,size,name,decompiled");
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            PrintWriter chunk = null;
            int n = 0;
            while (it.hasNext() && !monitor.isCancelled()) {
                Function f = it.next();
                if (n % 500 == 0) {
                    if (chunk != null) chunk.close();
                    chunk = new PrintWriter(new File(outDir, String.format("decomp_%04d.c", n / 500)), "UTF-8");
                }
                DecompileResults r = ifc.decompileFunction(f, 60, monitor);
                boolean ok = r != null && r.decompileCompleted();
                chunk.printf("// ==== %s @ %s ====%n", f.getName(), f.getEntryPoint());
                chunk.println(ok ? r.getDecompiledFunction().getC() : "// decompilation failed: " + (r == null ? "null" : r.getErrorMessage()));
                list.printf("%s,%d,%s,%s%n", f.getEntryPoint(), f.getBody().getNumAddresses(), f.getName(), ok);
                if (++n % 250 == 0) println("decompiled " + n);
            }
            if (chunk != null) chunk.close();
            println("total functions: " + n);
        }
        ifc.dispose();
    }
}
