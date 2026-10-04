package imgui.il2cpp.tool;

import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.graphics.PixelFormat;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Display;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.graphics.Point;

import java.lang.reflect.Field;
import java.util.Map;

/**
 * 注入场景下的 overlay 入口。
 * 通过反射获取当前 Activity，在主线程创建全屏透明 GLSurfaceView，
 * 由 GlViewWrapper 的 Renderer 回调驱动 native OpenGL 渲染。
 * 监听 Activity 生命周期，Activity 切换时自动重新绑定 overlay。
 */
public class OverlayHelper {

    private static GlViewWrapper sGlView;
    private static Activity sCurrentActivity;
    private static boolean sStarted = false;
    private static Application.ActivityLifecycleCallbacks sLifecycleCallbacks;

    /** 通过反射获取当前栈顶 Activity（未 paused 的） */
    private static Activity getCurrentActivity() {
        try {
            Class<?> activityThreadClass = Class.forName("android.app.ActivityThread");
            Object activityThread = activityThreadClass.getMethod("currentActivityThread").invoke(null);
            Field activitiesField = activityThreadClass.getDeclaredField("mActivities");
            activitiesField.setAccessible(true);
            Map<?, ?> activities = (Map<?, ?>) activitiesField.get(activityThread);
            if (activities == null)
                return null;
            Activity fallback = null;
            for (Object activityRecord : activities.values()) {
                Class<?> recordClass = activityRecord.getClass();
                Field activityField = recordClass.getDeclaredField("activity");
                activityField.setAccessible(true);
                Activity activity = (Activity) activityField.get(activityRecord);
                if (activity == null)
                    continue;
                if (fallback == null)
                    fallback = activity;
                try {
                    Field pausedField = recordClass.getDeclaredField("paused");
                    pausedField.setAccessible(true);
                    if (!pausedField.getBoolean(activityRecord))
                        return activity;
                } catch (Exception ignored) {
                }
            }
            return fallback;
        } catch (Exception e) {
            e.printStackTrace();
        }
        return null;
    }

    /** native 侧调用：在游戏进程里启动 overlay */
    public static void start() {
        if (sStarted)
            return;
        sStarted = true;
        new Handler(Looper.getMainLooper()).post(() -> {
            // 注册 Activity 生命周期回调，在 onResume 时绑定 overlay
            try {
                Activity activity = getCurrentActivity();
                if (activity != null) {
                    Application app = activity.getApplication();
                    sLifecycleCallbacks = new Application.ActivityLifecycleCallbacks() {
                        @Override public void onActivityCreated(Activity a, Bundle savedInstanceState) {}
                        @Override public void onActivityStarted(Activity a) {}
                        @Override public void onActivityResumed(Activity a) {
                            // 前台 Activity 变化时重新绑定 overlay
                            if (a != sCurrentActivity || sGlView == null) {
                                android.util.Log.i("MXP", "onActivityResumed: rebind to " + a.getClass().getSimpleName());
                                removeOverlay();
                                sCurrentActivity = a;
                                // 等 DecorView 布局完成后再创建，避免拿不到 surface
                                a.getWindow().getDecorView().post(() -> createOverlay(a));
                            }
                        }
                        @Override public void onActivityPaused(Activity a) {}
                        @Override public void onActivityStopped(Activity a) {}
                        @Override public void onActivitySaveInstanceState(Activity a, Bundle outState) {}
                        @Override public void onActivityDestroyed(Activity a) {
                            if (a == sCurrentActivity) {
                                android.util.Log.i("MXP", "onActivityDestroyed: " + a.getClass().getSimpleName());
                                removeOverlay();
                                sCurrentActivity = null;
                            }
                        }
                    };
                    app.registerActivityLifecycleCallbacks(sLifecycleCallbacks);
                    // 立即在当前 Activity 上创建
                    sCurrentActivity = activity;
                    createOverlay(activity);
                } else {
                    // Activity 还没准备好，延迟重试
                    sStarted = false;
                    new Handler(Looper.getMainLooper()).postDelayed(() -> start(), 500);
                }
            } catch (Exception e) {
                e.printStackTrace();
                sStarted = false;
            }
        });
    }

    private static void createOverlay(Activity activity) {
        try {
            android.util.Log.i("MXP", "createOverlay: " + activity.getClass().getSimpleName());
            WindowManager wm = activity.getWindowManager();
            Display display = wm.getDefaultDisplay();
            Point size = new Point();
            display.getRealSize(size);

            Context displayContext = activity.createDisplayContext(display);
            sGlView = new GlViewWrapper(displayContext);
            sGlView.setOnTouchListener((v, event) -> {
                nativeOnTouch(event.getAction(), event.getX(), event.getY(),
                        event.getPointerId(0), event.getEventTime());
                return true;
            });

            WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                    size.x, size.y,
                    WindowManager.LayoutParams.TYPE_APPLICATION,
                    WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE |
                            WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS |
                            WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL,
                    PixelFormat.TRANSPARENT);
            lp.gravity = Gravity.TOP | Gravity.START;
            lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            lp.softInputMode = WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING;

            wm.addView(sGlView, lp);
            android.util.Log.i("MXP", "createOverlay: addView done " + size.x + "x" + size.y);
        } catch (Exception e) {
            android.util.Log.e("MXP", "createOverlay failed", e);
            sGlView = null;
        }
    }

    private static void removeOverlay() {
        if (sGlView != null && sCurrentActivity != null) {
            try {
                WindowManager wm = sCurrentActivity.getWindowManager();
                wm.removeView(sGlView);
            } catch (Exception ignored) {
            }
            sGlView = null;
        }
    }

    /** 触摸事件传递给 native（由 ImGui_ImplAndroid 处理） */
    public static native void nativeOnTouch(int action, float x, float y, int pointerId, long eventTime);
}
