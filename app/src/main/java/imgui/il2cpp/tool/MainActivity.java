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
import android.graphics.Rect;
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
        lp.gravity = Gravity.TOP | Gravity.CENTER_HORIZONTAL | Gravity.CENTER_VERTICAL;
        lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        lp.softInputMode = WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING;

        GlViewWrapper view = new GlViewWrapper(ctx);

        Display display = wm.getDefaultDisplay();
        Point size = new Point();
        display.getRealSize(size);
        int screenWidth = size.x;
        int screenHeight = size.y;


        Rect rect = new Rect();
        view.getWindowVisibleDisplayFrame(rect);
        int statusBarHeight = rect.top;

        lp.width = screenWidth;
        lp.height = screenHeight - statusBarHeight;

        wm.addView(view, lp);
    }
}
