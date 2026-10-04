package imgui.il2cpp.tool;

import android.content.Context;
import android.graphics.PixelFormat;
import android.opengl.GLSurfaceView;
import android.util.Log;
import android.view.SurfaceHolder;

import javax.microedition.khronos.egl.EGL10;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.egl.EGLContext;
import javax.microedition.khronos.egl.EGLDisplay;
import javax.microedition.khronos.opengles.GL10;

/**
 * GLSurfaceView 包装，负责 EGL/GLES3 上下文创建和 native 渲染回调。
 * 参考 IL2CPP MCP：检查 GLES3 可用性，失败时记录错误并禁用 overlay（不崩溃）。
 */
public class GlViewWrapper extends GLSurfaceView implements GLSurfaceView.Renderer, SurfaceHolder.Callback2 {

    private static final String TAG = "MXP";

    public GlViewWrapper(Context context) {
        super(context);
        setEGLContextClientVersion(3);
        setEGLConfigChooser(8, 8, 8, 8, 16, 0);
        // 自定义 EGLContextFactory，捕获上下文创建失败，GLES3 失败时回退 GLES2
        setEGLContextFactory(new ContextFactory());
        getHolder().setFormat(PixelFormat.TRANSPARENT);
        setZOrderOnTop(true);
        setRenderer(this);
    }

    @Override
    public void onSurfaceCreated(GL10 gl, EGLConfig config) {
        Log.i(TAG, "GlViewWrapper.onSurfaceCreated");
        NativeMethods.onSurfaceCreate();
    }

    @Override
    public void onSurfaceChanged(GL10 gl, int width, int height) {
        Log.i(TAG, "GlViewWrapper.onSurfaceChanged: " + width + "x" + height);
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
    public void surfaceCreated(SurfaceHolder holder) {
        Log.i(TAG, "GlViewWrapper.surfaceCreated");
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        Log.i(TAG, "GlViewWrapper.surfaceDestroyed: notify native GL context lost");
        OverlayHelper.onSurfaceDestroyed();
    }

    /** 自定义 EGLContextFactory，捕获上下文创建失败，避免崩溃 */
    private static class ContextFactory implements GLSurfaceView.EGLContextFactory {
        private static final int EGL_CONTEXT_CLIENT_VERSION = 0x3098;

        @Override
        public EGLContext createContext(EGL10 egl, EGLDisplay display, EGLConfig eglConfig) {
            Log.i(TAG, "EGL createContext: requesting GLES3");
            int[] attrib_list = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL10.EGL_NONE};
            EGLContext context = egl.eglCreateContext(display, eglConfig, EGL10.EGL_NO_CONTEXT, attrib_list);
            if (context == null || context == EGL10.EGL_NO_CONTEXT) {
                Log.e(TAG, "EGL createContext GLES3 failed, error=0x" + Integer.toHexString(egl.eglGetError()));
                // 回退到 GLES2
                int[] attrib_list2 = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL10.EGL_NONE};
                context = egl.eglCreateContext(display, eglConfig, EGL10.EGL_NO_CONTEXT, attrib_list2);
                if (context == null || context == EGL10.EGL_NO_CONTEXT) {
                    Log.e(TAG, "EGL createContext GLES2 also failed, error=0x" + Integer.toHexString(egl.eglGetError()));
                } else {
                    Log.w(TAG, "EGL createContext fell back to GLES2");
                }
            } else {
                Log.i(TAG, "EGL createContext GLES3 success");
            }
            return context;
        }

        @Override
        public void destroyContext(EGL10 egl, EGLDisplay display, EGLContext context) {
            if (!egl.eglDestroyContext(display, context)) {
                Log.e(TAG, "EGL destroyContext failed, error=0x" + Integer.toHexString(egl.eglGetError()));
            }
        }
    }
}
