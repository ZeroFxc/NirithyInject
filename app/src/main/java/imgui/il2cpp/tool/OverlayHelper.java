package imgui.il2cpp.tool;

import android.app.Activity;
import android.content.Context;
import android.graphics.PixelFormat;
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
 */
public class OverlayHelper {

    private static GlViewWrapper sGlView;
    private static boolean sStarted = false;

    /** 通过反射获取当前栈顶 Activity */
    private static Activity getCurrentActivity() {
        try {
            Class<?> activityThreadClass = Class.forName("android.app.ActivityThread");
            Object activityThread = activityThreadClass.getMethod("currentActivityThread").invoke(null);
            Field activitiesField = activityThreadClass.getDeclaredField("mActivities");
            activitiesField.setAccessible(true);
            Map<?, ?> activities = (Map<?, ?>) activitiesField.get(activityThread);
            if (activities == null)
                return null;
            for (Object activityRecord : activities.values()) {
                Class<?> recordClass = activityRecord.getClass();
                Field activityField = recordClass.getDeclaredField("activity");
                activityField.setAccessible(true);
                Activity activity = (Activity) activityField.get(activityRecord);
                if (activity != null)
                    return activity;
            }
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
        // 延迟到主线程执行，等待 Activity 就绪
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            Activity activity = getCurrentActivity();
            if (activity == null) {
                // Activity 还没准备好，重试
                sStarted = false;
                new Handler(Looper.getMainLooper()).postDelayed(() -> start(), 500);
                return;
            }
            createOverlay(activity);
        }, 300);
    }

    private static void createOverlay(Activity activity) {
        try {
            WindowManager wm = activity.getWindowManager();
            Display display = wm.getDefaultDisplay();
            Point size = new Point();
            display.getRealSize(size);

            Context displayContext = activity.createDisplayContext(display);
            sGlView = new GlViewWrapper(displayContext);
            // 触摸事件传给 native
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
        } catch (Exception e) {
            e.printStackTrace();
            sStarted = false;
        }
    }

    /** 触摸事件传递给 native（由 ImGui_ImplAndroid 处理） */
    public static native void nativeOnTouch(int action, float x, float y, int pointerId, long eventTime);
}
