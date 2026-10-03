package imgui.il2cpp.tool;


import android.app.Activity;
import android.content.Context;
import android.graphics.PixelFormat;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.view.Gravity;
import android.view.Display;
import android.graphics.Point;
import androidx.annotation.Keep;

public class MainActivity extends Activity {
    static {
        System.loadLibrary("Tool");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        startMain(this);
    }

    @Keep
    public static void startMain(Context ctx) {
        Handler handler = new Handler(Looper.getMainLooper());
        handler.postDelayed(() -> {
            startMenu(ctx);
        }, 100);
    }

    public static void startMenu(Context ctx) {
        WindowManager wm = ((Activity) ctx).getWindowManager();
        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                                                ViewGroup.LayoutParams.MATCH_PARENT,
                                                ViewGroup.LayoutParams.MATCH_PARENT,
                                                0, 0, 
                                                WindowManager.LayoutParams.TYPE_APPLICATION, 
                                                WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE | 
                                                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE |
                                                WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS, 
                                                PixelFormat.TRANSPARENT);
        // 固定左上角对齐：Waydroid/WSA 下窗口尺寸与可用区域可能不一致，
        // 居中对齐会把窗口整体推到屏幕外，导致叠加层位置错误
        lp.gravity = Gravity.TOP | Gravity.START;
        lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        lp.softInputMode = WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING;

        Display display = wm.getDefaultDisplay();
        Point size = new Point();
        display.getRealSize(size);
        int screenWidth = size.x;
        int screenHeight = size.y;

        // 密度修正：用 display 上下文创建 GL 视图，使窗口密度与 display 实际密度保持一致。
        // Waydroid/WSA 上 Activity 资源密度可能与 display 不同，直接使用 ctx 会导致
        // 窗口被合成器按错误密度缩放，叠加层随之偏移。
        Context displayContext = ctx.createDisplayContext(display);
        GlViewWrapper view = new GlViewWrapper(displayContext);

        lp.width = screenWidth;
        lp.height = screenHeight;

        wm.addView(view, lp);
    }
}
