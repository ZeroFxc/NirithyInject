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

    // 帧统计：用于诊断渲染是否在跑、帧率多少
    private long mFrameCount = 0;
    private long mFirstFrameTime = 0;
    private long mLastHeartbeatTime = 0;
    private long mLastFrameTime = 0;
    private float mFps = 0;

    public GlViewWrapper(Context context) {
        super(context);
        Log.i(TAG, "GlViewWrapper constructor: setting up GLES3 GLSurfaceView");
        setEGLContextClientVersion(3);
        setEGLConfigChooser(8, 8, 8, 8, 16, 0);
        // 自定义 EGLContextFactory，捕获上下文创建失败，GLES3 失败时回退 GLES2
        setEGLContextFactory(new ContextFactory());
        getHolder().setFormat(PixelFormat.TRANSPARENT);
        setZOrderOnTop(true);
        setRenderer(this);
        setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);
        Log.i(TAG, "GlViewWrapper constructor: setRenderer done, renderMode=CONTINUOUSLY");
        // 额外的 surface 销毁监听：不覆盖父类的 surfaceDestroyed，
        // 而是通过 addCallback 添加独立的回调，确保 GLSurfaceView 内部清理正常执行
        getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder holder) {
                Log.i(TAG, "GlViewWrapper extra callback: surfaceCreated holder=" + holder
                        + " surface=" + holder.getSurface()
                        + " valid=" + holder.getSurface().isValid());
            }
            @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                Log.i(TAG, "GlViewWrapper extra callback: surfaceChanged " + width + "x" + height + " fmt=" + format);
            }
            @Override public void surfaceDestroyed(SurfaceHolder holder) {
                Log.i(TAG, "GlViewWrapper extra callback: surfaceDestroyed, totalFrames=" + mFrameCount
                        + ", notify native GL context lost");
                OverlayHelper.onSurfaceDestroyed();
            }
        });
    }

    @Override
    public void onSurfaceCreated(GL10 gl, EGLConfig config) {
        Log.i(TAG, "GlViewWrapper.onSurfaceCreated (Renderer)");
        // 查询 GL 版本/渲染器，确认 EGL 上下文正确
        try {
            String glVersion = gl.glGetString(GL10.GL_VERSION);
            String glRenderer = gl.glGetString(GL10.GL_RENDERER);
            String glVendor = gl.glGetString(GL10.GL_VENDOR);
            Log.i(TAG, "GL info: version=" + glVersion + " renderer=" + glRenderer + " vendor=" + glVendor);
        } catch (Exception e) {
            Log.w(TAG, "GL info query failed: " + e.getMessage());
        }
        mFrameCount = 0;
        mFirstFrameTime = System.nanoTime();
        mLastHeartbeatTime = mFirstFrameTime;
        mLastFrameTime = mFirstFrameTime;
        NativeMethods.onSurfaceCreate();
    }

    @Override
    public void onSurfaceChanged(GL10 gl, int width, int height) {
        Log.i(TAG, "GlViewWrapper.onSurfaceChanged: " + width + "x" + height);
        NativeMethods.onSurfaceChanged(width, height);
    }

    @Override
    public void onDrawFrame(GL10 gl) {
        long now = System.nanoTime();
        mFrameCount++;
        // 计算瞬时 FPS（滑动平均）
        if (mLastFrameTime > 0) {
            float instantFps = 1e9f / (now - mLastFrameTime);
            mFps = mFps == 0 ? instantFps : mFps * 0.9f + instantFps * 0.1f;
        }
        mLastFrameTime = now;
        // 首次帧日志
        if (mFrameCount == 1) {
            Log.i(TAG, "onDrawFrame: FIRST CALL, elapsed since surfaceCreated="
                    + ((now - mFirstFrameTime) / 1_000_000) + "ms");
        }
        // 每 300 帧（约5秒@60fps）打一次心跳，证明渲染循环在跑
        if (mFrameCount % 300 == 0) {
            long elapsedSec = (now - mLastHeartbeatTime) / 1_000_000_000;
            Log.i(TAG, "onDrawFrame: heartbeat frame=" + mFrameCount
                    + " fps=" + String.format("%.1f", mFps)
                    + " elapsed=" + elapsedSec + "s");
            mLastHeartbeatTime = now;
        }
        NativeMethods.onDrawFrame();
    }

    /** 自定义 EGLContextFactory，捕获上下文创建失败，避免崩溃 */
    private static class ContextFactory implements GLSurfaceView.EGLContextFactory {
        private static final int EGL_CONTEXT_CLIENT_VERSION = 0x3098;

        private static String eglErrorString(int error) {
            switch (error) {
                case EGL10.EGL_SUCCESS: return "SUCCESS";
                case EGL10.EGL_NOT_INITIALIZED: return "NOT_INITIALIZED";
                case EGL10.EGL_BAD_ACCESS: return "BAD_ACCESS";
                case EGL10.EGL_BAD_ALLOC: return "BAD_ALLOC";
                case EGL10.EGL_BAD_ATTRIBUTE: return "BAD_ATTRIBUTE";
                case EGL10.EGL_BAD_CONFIG: return "BAD_CONFIG";
                case EGL10.EGL_BAD_CONTEXT: return "BAD_CONTEXT";
                case EGL10.EGL_BAD_CURRENT_SURFACE: return "BAD_CURRENT_SURFACE";
                case EGL10.EGL_BAD_DISPLAY: return "BAD_DISPLAY";
                case EGL10.EGL_BAD_MATCH: return "BAD_MATCH";
                case EGL10.EGL_BAD_NATIVE_PIXMAP: return "BAD_NATIVE_PIXMAP";
                case EGL10.EGL_BAD_NATIVE_WINDOW: return "BAD_NATIVE_WINDOW";
                case EGL10.EGL_BAD_PARAMETER: return "BAD_PARAMETER";
                case EGL10.EGL_BAD_SURFACE: return "BAD_SURFACE";
                default: return "UNKNOWN(0x" + Integer.toHexString(error) + ")";
            }
        }

        @Override
        public EGLContext createContext(EGL10 egl, EGLDisplay display, EGLConfig eglConfig) {
            Log.i(TAG, "EGL createContext: display=" + display + " config=" + eglConfig + " requesting GLES3");
            int[] attrib_list = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL10.EGL_NONE};
            EGLContext context = egl.eglCreateContext(display, eglConfig, EGL10.EGL_NO_CONTEXT, attrib_list);
            int err = egl.eglGetError();
            if (context == null || context == EGL10.EGL_NO_CONTEXT) {
                Log.e(TAG, "EGL createContext GLES3 failed: " + eglErrorString(err));
                // 回退到 GLES2
                int[] attrib_list2 = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL10.EGL_NONE};
                context = egl.eglCreateContext(display, eglConfig, EGL10.EGL_NO_CONTEXT, attrib_list2);
                err = egl.eglGetError();
                if (context == null || context == EGL10.EGL_NO_CONTEXT) {
                    Log.e(TAG, "EGL createContext GLES2 also failed: " + eglErrorString(err));
                } else {
                    Log.w(TAG, "EGL createContext fell back to GLES2 (imgui shaders may fail)");
                }
            } else {
                Log.i(TAG, "EGL createContext GLES3 success: context=" + context);
            }
            return context;
        }

        @Override
        public void destroyContext(EGL10 egl, EGLDisplay display, EGLContext context) {
            Log.i(TAG, "EGL destroyContext: context=" + context);
            if (!egl.eglDestroyContext(display, context)) {
                Log.e(TAG, "EGL destroyContext failed: " + eglErrorString(egl.eglGetError()));
            }
        }
    }
}
