// Dump the whole program as C (parallel decompile) to the path given as arg 0.
import ghidra.app.script.GhidraScript;
import ghidra.app.util.exporter.CppExporter;
import java.io.File;

public class ExportC extends GhidraScript {
    @Override public void run() throws Exception {
        CppExporter e = new CppExporter();
        java.util.List<ghidra.app.util.Option> opts = e.getOptions(() -> currentProgram);
        for (ghidra.app.util.Option o : opts) {
            if (o.getName().equals(CppExporter.CREATE_HEADER_FILE)) o.setValue(Boolean.FALSE);
        }
        e.setOptions(opts);
        e.export(new File(getScriptArgs()[0]), currentProgram, null, monitor);
    }
}
