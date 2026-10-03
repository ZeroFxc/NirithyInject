package imgui.il2cpp.tool;

public class NativeMethods {
    public static native void onDrawFrame();

    public static native void onSurfaceChanged(int width, int height);

    public static native void onSurfaceCreate();
}
