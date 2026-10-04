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
import android.view.Window;
import android.view.WindowManager;
import android.graphics.Point;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.Map;

/**
 * 注入场景下的 overlay 入口。
 * 参考 IL2CPP MCP v2.7.9 的 OverlayBridge 实现：
 * - findResumedActivity 优先用 ActivityThread.mResumedActivity
 * - Window touch routing 优先 hook Window.dispatchTouchEvent，失败回退 SurfaceView listener
 * - surfaceDestroyed 通知 native 重置 GL 状态（EGL 上下文丢失）
 * - ActivityLifecycleCallbacks 监听切换，onResume 时重新绑定
 */
public class OverlayHelper {

    private static GlViewWrapper sGlView;
    private static Activity sCurrentActivity;
    private static boolean sStarted = false;
    private static Application.ActivityLifecycleCallbacks sLifecycleCallbacks;
    private static boolean sWindowTouchInstalled = false;
    private static Window.Callback sOriginalWindowCallback;

    /** 优先用 ActivityThread.mResumedActivity，失败回退遍历 mActivities */
    private static Activity findResumedActivity() {
        try {
            Class<?> activityThreadClass = Class.forName("android.app.ActivityThread");
            Object activityThread = activityThreadClass.getMethod("currentActivityThread").invoke(null);

            // 优先：直接读 mResumedActivity 字段（Android 7.0+）
            try {
                Field resumedField = activityThreadClass.getDeclaredField("mResumedActivity");
                resumedField.setAccessible(true);
                Object activityRecord = resumedField.get(activityThread);
                if (activityRecord != null) {
                    Field activityField = activityRecord.getClass().getDeclaredField("activity");
                    activityField.setAccessible(true);
                    Activity activity = (Activity) activityField.get(activityRecord);
                    if (activity != null) {
                        android.util.Log.i("MXP", "findResumedActivity: mResumedActivity=" + activity.getClass().getSimpleName());
                        return activity;
                    }
                }
            } catch (Exception ignored) {
            }

            // 回退：遍历 mActivities 找未 paused 的
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
                    if (!pausedField.getBoolean(activityRecord)) {
                        android.util.Log.i("MXP", "findResumedActivity: mActivities scan=" + activity.getClass().getSimpleName());
                        return activity;
                    }
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
            try {
                // 获取 Application（通过当前 Activity 或反射）
                Application app = getApplication();
                if (app == null) {
                    android.util.Log.w("MXP", "start: Application not available, retry in 500ms");
                    sStarted = false;
                    new Handler(Looper.getMainLooper()).postDelayed(() -> start(), 500);
                    return;
                }

                // 注册 Activity 生命周期回调（参考 IL2CPP MCP：overlay bridge ready; waiting for a foreground Activity）
                sLifecycleCallbacks = new Application.ActivityLifecycleCallbacks() {
                    @Override public void onActivityCreated(Activity a, Bundle savedInstanceState) {}
                    @Override public void onActivityStarted(Activity a) {}
                    @Override public void onActivityResumed(Activity a) {
                        android.util.Log.i("MXP", "onActivityResumed: " + a.getClass().getSimpleName()
                                + " current=" + (sCurrentActivity != null ? sCurrentActivity.getClass().getSimpleName() : "null")
                                + " glView=" + (sGlView != null ? "attached" : "null"));
                        bindToActivity(a);
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
                android.util.Log.i("MXP", "overlay bridge ready; waiting for a foreground Activity");

                // 手动绑定到当前 resumed Activity（registerCallbacks 不会对已 resumed 的 Activity 触发回调）
                Activity activity = findResumedActivity();
                if (activity != null) {
                    bindToActivity(activity);
                } else {
                    android.util.Log.i("MXP", "start: no resumed activity yet, waiting for onActivityResumed");
                }
            } catch (Exception e) {
                e.printStackTrace();
                sStarted = false;
            }
        });
    }

    /** 绑定 overlay 到指定 Activity：移除旧 overlay，延迟创建新 overlay */
    private static void bindToActivity(Activity a) {
        if (a == sCurrentActivity && sGlView != null) {
            android.util.Log.i("MXP", "bindToActivity: already bound to " + a.getClass().getSimpleName());
            return;
        }
        removeOverlay();
        sCurrentActivity = a;
        // 等 DecorView 布局完成后再创建，避免拿不到 surface
        // 用 postDelayed 确保 Activity 完全 resume（参考 IL2CPP MCP 的 waiting for activity resume）
        a.getWindow().getDecorView().postDelayed(() -> {
            if (sCurrentActivity == a) {
                createOverlay(a);
            } else {
                android.util.Log.i("MXP", "bindToActivity: activity changed before createOverlay, skip");
            }
        }, 100);
    }

    /** 通过反射获取 Application（ActivityThread.currentApplication） */
    private static Application getApplication() {
        try {
            Class<?> activityThreadClass = Class.forName("android.app.ActivityThread");
            Object activityThread = activityThreadClass.getMethod("currentActivityThread").invoke(null);
            Method currentApplicationMethod = activityThreadClass.getMethod("currentApplication");
            return (Application) currentApplicationMethod.invoke(activityThread);
        } catch (Exception e) {
            // 回退：通过 findResumedActivity 获取
            Activity activity = findResumedActivity();
            return activity != null ? activity.getApplication() : null;
        }
    }

    private static void createOverlay(Activity activity) {
        try {
            android.util.Log.i("MXP", "createOverlay: " + activity.getClass().getSimpleName()
                    + " isFinishing=" + activity.isFinishing()
                    + " isDestroyed=" + activity.isDestroyed());
            if (activity.isFinishing() || activity.isDestroyed()) {
                android.util.Log.w("MXP", "createOverlay: activity finishing/destroyed, skip");
                return;
            }
            WindowManager wm = activity.getWindowManager();
            Display display = wm.getDefaultDisplay();
            Point size = new Point();
            display.getRealSize(size);

            Context displayContext = activity.createDisplayContext(display);
            sGlView = new GlViewWrapper(displayContext);
            android.util.Log.i("MXP", "createOverlay: GlViewWrapper created " + size.x + "x" + size.y);

            // 优先安装 Window touch routing（hook dispatchTouchEvent）
            installWindowTouch(activity);

            // SurfaceView listener 作为回退
            sGlView.setOnTouchListener((v, event) -> {
                if (!sWindowTouchInstalled) {
                    nativeOnTouch(event.getAction(), event.getX(), event.getY(),
                            event.getPointerId(0), event.getEventTime());
                }
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
            android.util.Log.i("MXP", "createOverlay: addView done " + size.x + "x" + size.y
                    + " windowTouch=" + sWindowTouchInstalled
                    + " glViewParent=" + (sGlView.getParent() != null ? "attached" : "null"));
        } catch (Exception e) {
            android.util.Log.e("MXP", "createOverlay failed", e);
            sGlView = null;
        }
    }

    /**
     * Window touch routing：用动态代理替换 Window.Callback，拦截 dispatchTouchEvent。
     * 这样 overlay 外面的触摸也能被捕获（IL2CPP MCP 的方案）。
     * 失败时回退到 SurfaceView 的 OnTouchListener。
     */
    private static void installWindowTouch(Activity activity) {
        try {
            Window window = activity.getWindow();
            sOriginalWindowCallback = window.getCallback();
            final Window.Callback original = sOriginalWindowCallback;

            // 动态代理：只拦截 dispatchTouchEvent，其余方法反射调用原始 callback
            Window.Callback proxy = (Window.Callback) java.lang.reflect.Proxy.newProxyInstance(
                    Window.Callback.class.getClassLoader(),
                    new Class<?>[]{Window.Callback.class},
                    (proxy1, method, args) -> {
                        if ("dispatchTouchEvent".equals(method.getName()) && args != null && args.length > 0) {
                            MotionEvent event = (MotionEvent) args[0];
                            nativeOnTouch(event.getAction(), event.getX(), event.getY(),
                                    event.getPointerId(0), event.getEventTime());
                        }
                        if (original != null) {
                            try {
                                return method.invoke(original, args);
                            } catch (java.lang.reflect.InvocationTargetException e) {
                                throw e.getTargetException();
                            }
                        }
                        return null;
                    });
            window.setCallback(proxy);
            sWindowTouchInstalled = true;
            android.util.Log.i("MXP", "Window touch routing installed (dynamic proxy)");
        } catch (Exception e) {
            android.util.Log.w("MXP", "Window touch routing unavailable; using SurfaceView listener", e);
            sWindowTouchInstalled = false;
        }
    }

    private static void restoreWindowTouch(Activity activity) {
        if (sWindowTouchInstalled && sOriginalWindowCallback != null && activity != null) {
            try {
                activity.getWindow().setCallback(sOriginalWindowCallback);
                android.util.Log.i("MXP", "Window touch routing restored");
            } catch (Exception ignored) {
            }
        }
        sWindowTouchInstalled = false;
        sOriginalWindowCallback = null;
    }

    private static void removeOverlay() {
        if (sGlView != null && sCurrentActivity != null) {
            try {
                restoreWindowTouch(sCurrentActivity);
                // 先通知 native GL 上下文丢失，让渲染线程跳过渲染（避免 removeView 时竞态崩溃）
                nativeOnSurfaceDestroyed();
                // 暂停 GL 线程，等待当前帧完成
                sGlView.onPause();
                WindowManager wm = sCurrentActivity.getWindowManager();
                wm.removeView(sGlView);
                android.util.Log.i("MXP", "removeOverlay: GL paused + view removed");
            } catch (Exception ignored) {
            }
            sGlView = null;
        }
    }

    /** GlViewWrapper surface 销毁时调用，通知 native GL 上下文丢失 */
    public static void onSurfaceDestroyed() {
        android.util.Log.i("MXP", "OverlayHelper.onSurfaceDestroyed: notify native GL context lost");
        nativeOnSurfaceDestroyed();
    }

    /** 触摸事件传递给 native（由 ImGui_ImplAndroid 处理） */
    public static native void nativeOnTouch(int action, float x, float y, int pointerId, long eventTime);
    /** GL 上下文丢失通知（EGL surface destroyed） */
    public static native void nativeOnSurfaceDestroyed();
}
