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
 * 注意：GLSurfaceView 自身已实现 SurfaceHolder.Callback2，子类不可重写
 * surfaceCreated/surfaceChanged/surfaceDestroyed 而不调用 super，
 * 否则会跳过内部 EGL 初始化，导致 !readyToDraw() 且 Renderer 回调永不触发。
 * surfaceDestroyed 通知通过额外的 Callback 实现，不覆盖父类方法。
 */
public class GlViewWrapper extends GLSurfaceView implements GLSurfaceView.Renderer {

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
        // 额外的 surface 销毁监听：不覆盖父类的 surfaceDestroyed，
        // 而是通过 addCallback 添加独立的回调，确保 GLSurfaceView 内部清理正常执行
        getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder holder) {}
            @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}
            @Override public void surfaceDestroyed(SurfaceHolder holder) {
                Log.i(TAG, "GlViewWrapper surfaceDestroyed (extra callback): notify native GL context lost");
                OverlayHelper.onSurfaceDestroyed();
            }
        });
    }

    @Override
    public void onSurfaceCreated(GL10 gl, EGLConfig config) {
        Log.i(TAG, "GlViewWrapper.onSurfaceCreated (Renderer)");
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
