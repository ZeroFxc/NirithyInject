package imgui.il2cpp.tool;

import android.content.Context;
import android.graphics.PixelFormat;
import android.opengl.GLSurfaceView;
import android.view.SurfaceHolder;

import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

public class GlViewWrapper extends GLSurfaceView implements GLSurfaceView.Renderer, SurfaceHolder.Callback2 {

    public GlViewWrapper(Context context) {
        super(context);
        setEGLContextClientVersion(3);
        setEGLConfigChooser(8, 8, 8, 8, 16, 0);
        getHolder().setFormat(PixelFormat.TRANSPARENT);
        setZOrderOnTop(true);
        setRenderer(this);
    }

    @Override
    public void onSurfaceCreated(GL10 gl, EGLConfig config) {
        NativeMethods.onSurfaceCreate();
    }

    @Override
    public void onSurfaceChanged(GL10 gl, int width, int height) {
        NativeMethods.onSurfaceChanged(width, height);
    }

    @Override
    public void onDrawFrame(GL10 gl) {
        NativeMethods.onDrawFrame();
    }

    // SurfaceHolder.Callback2 — 通知 native GL 上下文丢失
    @Override
    public void surfaceRedrawNeeded(SurfaceHolder holder) {}

    @Override
    public void surfaceCreated(SurfaceHolder holder) {}

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        // EGL 上下文即将丢失，通知 native 重置 OpenGL3 backend 状态
        OverlayHelper.onSurfaceDestroyed();
    }
}
