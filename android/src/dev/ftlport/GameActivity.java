package dev.ftlport;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Insets;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.CombinedVibration;
import android.os.Handler;
import android.os.Looper;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.text.InputType;
import android.util.DisplayMetrics;
import android.view.Display;
import android.view.DisplayCutout;
import android.view.Gravity;
import android.view.HapticFeedbackConstants;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;

public class GameActivity extends Activity implements SurfaceHolder.Callback {
    private static final int REQ_ARCHIVE = 1, REQ_TREE = 2, REQ_LOGS = 3;
    private static boolean sNativeStarted;  // the game runs once per process

    private SurfaceView mSurface;
    private boolean mLibLoaded;
    private AlertDialog mTextDialog;
    private AlertDialog mQuitDialog;
    private boolean mExiting;
    private final Handler mMain = new Handler(Looper.getMainLooper());

    // ---- native ----
    private native void nativeStart(String bin, String bundle, String docs, String cache, String logs, String mods,
                                    String hyperspace, int sw, int sh, int refresh, float[] layout);
    private native void nativeAttach();
    private native void nativeSurfaceChanged(Surface s, int w, int h);
    private native void nativeSurfaceDestroyed();
    private native void nativeTouch(int phase, int id, float x, float y);
    private native void nativePause();
    private native void nativeResume();
    private native void nativeLowMemory();
    private native void nativeBack();
    private native void nativeQuit();
    private native void nativeTextResult(String text, boolean ok);
    private native void nativeAlertResult(int button);
    private native void nativeSetOption(String key, float value);
    private native void nativeKey(int keyCode, boolean down);
    private native void nativeMouse(int action, float x, float y, float wheel);
    private native void nativePadConnected(boolean connected, String name);
    private native void nativePadButton(int keyCode, boolean down);
    private native void nativePadAxes(float[] axes);
    private native void nativePadTrackpad(int fingers, float x, float y, boolean pressed);

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Logs.installCrashHandler(this);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(lp);
        }
        getWindow().getDecorView().setBackgroundColor(Color.BLACK);
        Uri game = incomingGame(getIntent());
        if (Importer.isInstalled(this)) {
            startGame();
        } else if (game != null) {
            showImporter(null);
            runImport(false, game);
        } else {
            tryAutoImportThenShowImporter();
        }
    }

    // The .ipa someone opened with FTL (passed on by ModsActivity).
    private static Uri incomingGame(Intent i) {
        return i != null && Intent.ACTION_VIEW.equals(i.getAction()) ? i.getData() : null;
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        Uri game = incomingGame(intent);
        if (game != null && mSurface == null && mPickArchive != null && mPickArchive.isEnabled()) runImport(false, game);
    }

    // ------------------------------------------------------------------ import UI
    private TextView mStatus;
    private ProgressBar mProgress;
    private Button mPickArchive, mPickFolder;

    private void showImporter(String message) {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setGravity(Gravity.CENTER);
        int pad = dp(24);
        col.setPadding(pad, pad, pad, pad);
        TextView title = new TextView(this);
        title.setText(R.string.import_title);
        title.setTextSize(24);
        title.setTextColor(0xFFE8F5E9);
        title.setGravity(Gravity.CENTER);
        col.addView(title);
        TextView body = new TextView(this);
        body.setText(message != null ? message : getString(R.string.import_body));
        body.setTextSize(16);
        body.setTextColor(0xFFB0BEC5);
        body.setGravity(Gravity.CENTER);
        body.setPadding(0, dp(16), 0, dp(16));
        col.addView(body);
        mPickArchive = makeButton(getString(R.string.import_pick_ipa), v -> pickArchive());
        mPickFolder = makeButton(getString(R.string.import_pick_folder), v -> pickFolder());
        col.addView(mPickArchive);
        col.addView(mPickFolder);
        col.addView(makeButton(getString(R.string.settings_short),
                v -> startActivity(new Intent(this, SettingsActivity.class))));
        mProgress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        mProgress.setVisibility(View.GONE);
        mProgress.setMax(1000);
        col.addView(mProgress, new LinearLayout.LayoutParams(dp(360), LinearLayout.LayoutParams.WRAP_CONTENT));
        mStatus = new TextView(this);
        mStatus.setTextColor(0xFF90A4AE);
        mStatus.setGravity(Gravity.CENTER);
        col.addView(mStatus);
        ScrollView sv = new ScrollView(this);
        sv.setFillViewport(true);
        sv.setBackgroundColor(0xFF0B1016);
        sv.addView(col);
        setContentView(sv);
    }

    private Button makeButton(String label, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setOnClickListener(l);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(360), LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(8);
        b.setLayoutParams(lp);
        return b;
    }

    private void pickArchive() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, REQ_ARCHIVE);
    }

    private void pickFolder() {
        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_TREE);
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (res != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        if (req == REQ_LOGS) saveLogs(uri);
        else runImport(req == REQ_TREE, uri);
    }

    private void tryAutoImportThenShowImporter() {
        showImporter(null);
        setBusy(true, getString(R.string.import_checking));
        new Thread(() -> {
            boolean ok = false;
            String err = null;
            try {
                ok = Importer.importFromExternalDir(this, this::progress);
            } catch (Exception e) {
                err = e.getMessage();
            }
            final boolean fok = ok;
            final String ferr = err;
            mMain.post(() -> {
                if (fok) startGame();
                else {
                    setBusy(false, ferr);
                }
            });
        }, "import").start();
    }

    private void runImport(boolean tree, Uri uri) {
        setBusy(true, getString(R.string.import_copying));
        new Thread(() -> {
            String err = null;
            try {
                if (tree) Importer.importTree(this, uri, this::progress);
                else Importer.importArchive(this, uri, this::progress);
            } catch (Exception e) {
                err = e.getMessage();
            }
            final String ferr = err;
            mMain.post(() -> {
                if (ferr == null && Importer.isInstalled(this)) startGame();
                else setBusy(false, ferr != null ? ferr : getString(R.string.import_failed));
            });
        }, "import").start();
    }

    private long mLastProgressUi;
    private void progress(String what, long done, long total) {
        long now = System.currentTimeMillis();
        if (now - mLastProgressUi < 100) return;
        mLastProgressUi = now;
        mMain.post(() -> {
            if (mProgress == null) return;
            mProgress.setIndeterminate(total <= 0);
            if (total > 0) mProgress.setProgress((int) (done * 1000 / total));
            mStatus.setText(getString(R.string.import_progress, what, done >> 20));
        });
    }

    private void setBusy(boolean busy, String status) {
        if (mPickArchive == null) return;
        mPickArchive.setEnabled(!busy);
        mPickFolder.setEnabled(!busy);
        mProgress.setVisibility(busy ? View.VISIBLE : View.GONE);
        mProgress.setIndeterminate(true);
        mStatus.setText(status != null ? status : "");
    }

    // ------------------------------------------------------------------ game
    private void startGame() {
        if (!mLibLoaded) {
            System.loadLibrary("ftlport");
            mLibLoaded = true;
        }
        mSurface = new SurfaceView(this);
        mSurface.getHolder().addCallback(this);
        mSurface.setOnTouchListener(this::onSurfaceTouch);
        mSurface.setOnCapturedPointerListener((v, e) -> onCapturedPointer(e));
        mSurface.setFocusable(true);
        mSurface.setFocusableInTouchMode(true);
        setContentView(mSurface);
        enterImmersive();
        if (sNativeStarted) nativeAttach();
    }

    private void enterImmersive() {
        View decor = getWindow().getDecorView();
        if (Build.VERSION.SDK_INT >= 30) {
            getWindow().setDecorFitsSystemWindows(false);
            WindowInsetsController c = decor.getWindowInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            decor.setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }

    // Show a dialog without bringing the system bars back: the dialog window
    // starts unfocusable (so it doesn't reset the bars), copies our immersive
    // state, then becomes focusable.
    private void showDialog(AlertDialog d) {
        android.view.Window w = d.getWindow();
        if (w == null) {
            d.show();
            return;
        }
        w.addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);
        d.show();
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = w.getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            w.getDecorView().setSystemUiVisibility(getWindow().getDecorView().getSystemUiVisibility());
        }
        w.clearFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);
    }

    @Override
    public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus && mSurface != null) enterImmersive();
        updateCapture();
    }

    private float[] computeLayout(int w, int h) {
        float l = 0, t = 0, r = 0, b = 0;
        WindowInsets wi = getWindow().getDecorView().getRootWindowInsets();
        if (wi != null && Build.VERSION.SDK_INT >= 28) {
            DisplayCutout dc = wi.getDisplayCutout();
            if (dc != null) {
                l = dc.getSafeInsetLeft();
                t = dc.getSafeInsetTop();
                r = dc.getSafeInsetRight();
                b = dc.getSafeInsetBottom();
            }
        }
        // Landscape game: only the short-edge (left/right) insets matter; keep full height.
        t = 0;
        b = 0;
        // The game's HUD stays inside a centred 4:3 area; beyond it there is only
        // background, which can run under a camera cutout. Inset only when a cutout
        // would reach the HUD, and then on both sides so a 180-degree turn is covered too.
        float side = Math.max(l, r), margin = (w - h * 4f / 3f) / 2f;
        if (side <= margin) {
            l = 0;
            r = 0;
        } else {
            l = side;
            r = side;
        }
        DisplayMetrics dm = getResources().getDisplayMetrics();
        SharedPreferences prefs = getSharedPreferences(SettingsActivity.PREFS, Context.MODE_PRIVATE);
        float quality = prefs.getFloat(SettingsActivity.K_RENDER, 0);
        float magnifier = prefs.getInt(SettingsActivity.K_MAGNIFIER, 0);
        return new float[] {l, t, r, b, dm.xdpi > 0 ? (dm.xdpi + dm.ydpi) / 2 : dm.densityDpi, quality, magnifier, 0};
    }

    // Touch options can change in SettingsActivity while the game runs.
    private void pushOptions() {
        SharedPreferences prefs = getSharedPreferences(SettingsActivity.PREFS, Context.MODE_PRIVATE);
        nativeSetOption("magnifier", prefs.getInt(SettingsActivity.K_MAGNIFIER, 0));
        nativeSetOption("two_finger_pause", prefs.getBoolean(SettingsActivity.K_TWO_FINGER, true) ? 1 : 0);
        nativeSetOption("minimap", prefs.getBoolean(SettingsActivity.K_MINIMAP, true) ? 1 : 0);
        nativeSetOption("haptics", prefs.getBoolean(SettingsActivity.K_HAPTICS, true) ? 1 : 0);
        nativeSetOption("event_text", prefs.getInt(SettingsActivity.K_EVENT_TEXT, 0));
        nativeSetOption("window_zoom", prefs.getInt(SettingsActivity.K_WINDOW_ZOOM, 0));
        nativeSetOption("event_zoom", prefs.getBoolean(SettingsActivity.K_EVENT_ZOOM, false) ? 1 : 0);
        nativeSetOption("pad_map_reset", 1);
        for (java.util.Map.Entry<Integer, Integer> e : PadMap.load(prefs).entrySet())
            nativeSetOption("pad_btn_" + e.getKey(), e.getValue());
        nativeSetOption("pad_magnetism", prefs.getBoolean(SettingsActivity.K_PAD_MAGNETISM, true) ? 1 : 0);
        nativeSetOption("pad_rumble", prefs.getBoolean(SettingsActivity.K_PAD_RUMBLE, true) ? 1 : 0);
        // Outlines of what the cursor is drawn to, for checking them: an empty file logs/pad-debug
        nativeSetOption("pad_debug", new File(getFilesDir(), "logs/pad-debug").exists() ? 1 : 0);
        mTouchpadOn = prefs.getBoolean(SettingsActivity.K_PAD_TOUCHPAD, true);
        updateCapture();
    }

    private int refreshRate() {
        Display d = getWindowManager().getDefaultDisplay();
        return d != null ? Math.round(d.getRefreshRate()) : 60;
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {}

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) {
        if (!sNativeStarted) {
            // The game's layout is fixed when it starts, so wait for the landscape
            // surface: launched behind the lock screen, the first one is portrait.
            if (w < h) return;
            sNativeStarted = true;
            File game = Importer.gameDir(this);
            File docs = SaveTransfer.docsDir(this);
            File cache = new File(getCacheDir(), "ftl");
            File logs = Logs.dir(this);
            //noinspection ResultOfMethodCallIgnored
            docs.mkdirs();
            //noinspection ResultOfMethodCallIgnored
            cache.mkdirs();
            OldSaves.repair(this);
            boolean crashed = Logs.crashedLastTime(this);
            Logs.clearCrashFlag(this);
            File mods = Mods.activeIndex(this);
            SharedPreferences prefs = getSharedPreferences(SettingsActivity.PREFS, Context.MODE_PRIVATE);
            File hyperspace = SettingsActivity.hyperspaceLib(this);
            boolean useHyperspace = hyperspace != null && prefs.getBoolean(SettingsActivity.K_HYPERSPACE, false);
            nativeStart(new File(game, "FTL").getPath(), game.getPath(), docs.getPath(), cache.getPath(),
                    logs.getPath(), mods != null ? mods.getPath() : "", useHyperspace ? hyperspace.getPath() : "",
                    w, h, refreshRate(), computeLayout(w, h));
            pushOptions();
            // After a crash, or once the mods changed, start at the title screen: the
            // saved run may be what crashed, or may not load with the new mods.
            boolean modsChanged = Mods.takeChanged(this);
            nativeSetOption("resume_run",
                    !crashed && !modsChanged && prefs.getBoolean(SettingsActivity.K_RESUME, false) ? 1 : 0);
            scanPads();
            if (crashed) mMain.postDelayed(this::offerCrashLogs, 1500);
        }
        // The game runs at 60 fps; let 90/120 Hz panels drop to 60 Hz while it's on screen.
        if (Build.VERSION.SDK_INT >= 30) {
            try {
                holder.getSurface().setFrameRate(60f, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);
            } catch (Exception ignored) {
            }
        }
        nativeSurfaceChanged(holder.getSurface(), w, h);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        nativeSurfaceDestroyed();
    }

    private int mMouseButtons;

    /** Desktop-style mouse (Chromebooks, DeX, Bluetooth mice): hover, left/right/middle, wheel. */
    private boolean onMouse(MotionEvent e) {
        float x = e.getX(), y = e.getY();
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_HOVER_MOVE:
            case MotionEvent.ACTION_MOVE:
                nativeMouse(0, x, y, 0);
                return true;
            case MotionEvent.ACTION_SCROLL:
                nativeMouse(7, x, y, e.getAxisValue(MotionEvent.AXIS_VSCROLL));
                return true;
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_BUTTON_PRESS:
            case MotionEvent.ACTION_BUTTON_RELEASE: {
                int now = e.getButtonState();
                int changed = now ^ mMouseButtons;
                if ((changed & MotionEvent.BUTTON_PRIMARY) != 0)
                    nativeMouse((now & MotionEvent.BUTTON_PRIMARY) != 0 ? 1 : 2, x, y, 0);
                if ((changed & MotionEvent.BUTTON_SECONDARY) != 0)
                    nativeMouse((now & MotionEvent.BUTTON_SECONDARY) != 0 ? 3 : 4, x, y, 0);
                if ((changed & MotionEvent.BUTTON_TERTIARY) != 0)
                    nativeMouse((now & MotionEvent.BUTTON_TERTIARY) != 0 ? 5 : 6, x, y, 0);
                mMouseButtons = now;
                return true;
            }
            default:
                return true;
        }
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if (mSurface != null && sNativeStarted && touchpadWithoutCapture(e)) return true;
        if (mSurface != null && sNativeStarted && e.isFromSource(android.view.InputDevice.SOURCE_MOUSE))
            return onMouse(e);
        if (mSurface != null && sNativeStarted && e.isFromSource(android.view.InputDevice.SOURCE_JOYSTICK)
                && e.getActionMasked() == MotionEvent.ACTION_MOVE) {
            mPadDevice = e.getDeviceId();
            mAxes[0] = e.getAxisValue(MotionEvent.AXIS_X);
            mAxes[1] = e.getAxisValue(MotionEvent.AXIS_Y);
            mAxes[2] = e.getAxisValue(MotionEvent.AXIS_Z);
            mAxes[3] = e.getAxisValue(MotionEvent.AXIS_RZ);
            mAxes[4] = Math.max(e.getAxisValue(MotionEvent.AXIS_LTRIGGER), e.getAxisValue(MotionEvent.AXIS_BRAKE));
            mAxes[5] = Math.max(e.getAxisValue(MotionEvent.AXIS_RTRIGGER), e.getAxisValue(MotionEvent.AXIS_GAS));
            mAxes[6] = e.getAxisValue(MotionEvent.AXIS_HAT_X);
            mAxes[7] = e.getAxisValue(MotionEvent.AXIS_HAT_Y);
            nativePadAxes(mAxes);
            return true;
        }
        return super.dispatchGenericMotionEvent(e);
    }

    private final float[] mAxes = new float[8];
    private volatile int mPadDevice = -1;  // the controller used last: it rumbles

    static boolean isGamepad(InputDevice d) {
        int src = d.getSources();
        return (src & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (src & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    // ---- DualSense / DualShock touchpad: moves the cursor like a laptop's (pad.c) ----
    // Android makes it a mouse pointer; pointer capture gives us the fingers instead. A
    // mouse used meanwhile gets the pointer back until the touchpad is touched again.
    private int mTouchpad = -1;
    private float mTouchpadW = 1, mTouchpadH = 1;
    private boolean mTouchpadOn = true, mMouseInUse;

    private void findTouchpad() {
        mTouchpad = -1;
        // Android 11 and older can't capture a touchpad: it stays a mouse, which works too
        if (Build.VERSION.SDK_INT < 31) return;
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null || d.isVirtual() || d.getVendorId() != 0x054c) continue;  // Sony
            // Android 12 and 13 make it a mouse until it's captured, and a touchpad after
            if (!d.supportsSource(InputDevice.SOURCE_TOUCHPAD) && !d.supportsSource(InputDevice.SOURCE_MOUSE)) continue;
            InputDevice.MotionRange x = d.getMotionRange(MotionEvent.AXIS_X, InputDevice.SOURCE_TOUCHPAD);
            InputDevice.MotionRange y = d.getMotionRange(MotionEvent.AXIS_Y, InputDevice.SOURCE_TOUCHPAD);
            mTouchpad = id;
            mTouchpadW = x != null && x.getRange() > 0 ? x.getRange() : 1920;
            mTouchpadH = y != null && y.getRange() > 0 ? y.getRange() : 1080;
            break;
        }
    }

    private void updateCapture() {
        if (mSurface == null) return;
        boolean want = mTouchpad >= 0 && mTouchpadOn && !mMouseInUse && hasWindowFocus() && sNativeStarted;
        if (want && !mSurface.hasPointerCapture()) mSurface.requestPointerCapture();
        else if (!want && mSurface.hasPointerCapture()) mSurface.releasePointerCapture();
    }

    private boolean onCapturedPointer(MotionEvent e) {
        if (e.getDeviceId() != mTouchpad || !e.isFromSource(InputDevice.SOURCE_TOUCHPAD)) {
            mMouseInUse = true;  // a mouse: give it the pointer
            updateCapture();
            return false;
        }
        int action = e.getActionMasked(), n = e.getPointerCount();
        int fingers = action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL ? 0
                : action == MotionEvent.ACTION_POINTER_UP ? n - 1 : n;
        boolean pressed = (e.getButtonState() & MotionEvent.BUTTON_PRIMARY) != 0;
        nativePadTrackpad(fingers, e.getX(0) / mTouchpadW, e.getY(0) / mTouchpadH, pressed);
        return true;
    }

    // The touchpad touched while the pointer was left to a mouse: take it back. (Newer
    // Androids make the controller and its touchpad one device: let its sticks through.)
    private boolean touchpadWithoutCapture(MotionEvent e) {
        if (mTouchpad < 0 || e.getDeviceId() != mTouchpad || !mTouchpadOn
                || !e.isFromSource(InputDevice.SOURCE_CLASS_POINTER)) return false;
        mMouseInUse = false;
        updateCapture();
        return true;
    }

    // Called from native (the game's thread): shake the controller, strength 0..1
    @SuppressWarnings("unused")
    void padRumble(float strength, int ms) {
        InputDevice d = mPadDevice >= 0 ? InputDevice.getDevice(mPadDevice) : null;
        if (d == null) return;
        int amp = Math.max(1, Math.min(255, Math.round(strength * 255)));
        try {
            if (Build.VERSION.SDK_INT >= 31) {
                VibratorManager vm = d.getVibratorManager();
                if (vm.getVibratorIds().length == 0) return;
                vm.vibrate(CombinedVibration.createParallel(VibrationEffect.createOneShot(ms, amp)));
            } else {
                Vibrator v = d.getVibrator();
                if (v != null && v.hasVibrator()) v.vibrate(VibrationEffect.createOneShot(ms, amp));
            }
        } catch (RuntimeException ignored) {
            // the controller went
        }
    }

    private static boolean isPadEvent(KeyEvent e) {
        int code = e.getKeyCode();
        if (KeyEvent.isGamepadButton(code)) return true;
        boolean dpad = code == KeyEvent.KEYCODE_DPAD_UP || code == KeyEvent.KEYCODE_DPAD_DOWN
                || code == KeyEvent.KEYCODE_DPAD_LEFT || code == KeyEvent.KEYCODE_DPAD_RIGHT;
        return dpad && (e.isFromSource(android.view.InputDevice.SOURCE_GAMEPAD)
                || e.isFromSource(android.view.InputDevice.SOURCE_JOYSTICK));
    }

    // Controllers drive the port's own cursor (pad.c): watch them come and go.
    private final android.hardware.input.InputManager.InputDeviceListener mPadListener =
            new android.hardware.input.InputManager.InputDeviceListener() {
                @Override public void onInputDeviceAdded(int id) { scanPads(); }
                @Override public void onInputDeviceRemoved(int id) { scanPads(); }
                @Override public void onInputDeviceChanged(int id) { scanPads(); }
            };
    private boolean mPadPresent;

    private void scanPads() {
        if (!sNativeStarted) return;
        String name = null;
        for (int id : android.view.InputDevice.getDeviceIds()) {
            android.view.InputDevice d = android.view.InputDevice.getDevice(id);
            if (d == null || d.isVirtual()) continue;
            if (isGamepad(d)) {
                name = d.getName();
                if (mPadDevice < 0 || InputDevice.getDevice(mPadDevice) == null) mPadDevice = id;
                break;
            }
        }
        findTouchpad();
        updateCapture();
        boolean present = name != null;
        if (present != mPadPresent) {
            mPadPresent = present;
            nativePadConnected(present, name);
        }
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        int code = e.getKeyCode();
        if (mSurface != null && sNativeStarted && mTextDialog == null && isPadEvent(e)) {
            mPadDevice = e.getDeviceId();
            if (e.getAction() == KeyEvent.ACTION_DOWN && e.getRepeatCount() == 0) nativePadButton(code, true);
            else if (e.getAction() == KeyEvent.ACTION_UP) nativePadButton(code, false);
            return true;
        }
        if (mSurface != null && sNativeStarted && code != KeyEvent.KEYCODE_BACK
                && code != KeyEvent.KEYCODE_VOLUME_UP && code != KeyEvent.KEYCODE_VOLUME_DOWN
                && code != KeyEvent.KEYCODE_VOLUME_MUTE && mTextDialog == null) {
            if (e.getAction() == KeyEvent.ACTION_DOWN && e.getRepeatCount() == 0) nativeKey(code, true);
            else if (e.getAction() == KeyEvent.ACTION_UP) nativeKey(code, false);
            return true;
        }
        return super.dispatchKeyEvent(e);
    }

    private boolean onSurfaceTouch(View v, MotionEvent e) {
        if (touchpadWithoutCapture(e)) return true;
        if (e.getToolType(0) == MotionEvent.TOOL_TYPE_MOUSE) {
            mMouseInUse = true;
            return onMouse(e);
        }
        int action = e.getActionMasked();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int i = e.getActionIndex();
                nativeTouch(0, e.getPointerId(i), e.getX(i), e.getY(i));
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++) nativeTouch(1, e.getPointerId(i), e.getX(i), e.getY(i));
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                int i = e.getActionIndex();
                nativeTouch(3, e.getPointerId(i), e.getX(i), e.getY(i));
                break;
            }
            case MotionEvent.ACTION_CANCEL:
                for (int i = 0; i < e.getPointerCount(); i++) nativeTouch(4, e.getPointerId(i), e.getX(i), e.getY(i));
                break;
            default:
                return false;
        }
        return true;
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK && mSurface != null) {
            nativeBack();
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        if (mSurface != null) nativeBack();
        else super.onBackPressed();
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (mSurface != null && sNativeStarted) nativePause();
        android.hardware.input.InputManager im = getSystemService(android.hardware.input.InputManager.class);
        if (im != null) im.unregisterInputDeviceListener(mPadListener);
    }

    @Override
    protected void onResume() {
        super.onResume();
        // Before the game has started too: on a cold start this runs first, and scanPads
        // waits for the game (surfaceChanged scans once it's up).
        android.hardware.input.InputManager im = getSystemService(android.hardware.input.InputManager.class);
        if (im != null) im.registerInputDeviceListener(mPadListener, mMain);
        if (mSurface != null && sNativeStarted) {
            pushOptions();
            nativeResume();
            enterImmersive();
            scanPads();
        }
    }

    @Override
    public void onTrimMemory(int level) {
        super.onTrimMemory(level);
        // Real memory pressure only (not merely being hidden / backgrounded).
        boolean pressure = level == TRIM_MEMORY_RUNNING_LOW || level == TRIM_MEMORY_RUNNING_CRITICAL
                || level >= TRIM_MEMORY_COMPLETE;
        if (pressure && sNativeStarted) nativeLowMemory();
    }

    // ------------------------------------------------------------------ called from native (any thread)
    @SuppressWarnings("unused")
    void requestTextInput(String prompt, String initial) {
        mMain.post(() -> {
            if (mTextDialog != null) mTextDialog.dismiss();
            EditText edit = new EditText(this);
            edit.setSingleLine(true);
            edit.setText(initial);
            edit.setSelection(edit.getText().length());
            edit.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_WORDS);
            edit.setImeOptions(EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_EXTRACT_UI);
            final boolean[] answered = {false};
            AlertDialog dlg = new AlertDialog.Builder(this)
                    .setTitle(prompt != null && !prompt.isEmpty() ? prompt : getString(R.string.text_input_title))
                    .setView(edit)
                    .setPositiveButton(android.R.string.ok, (d, w) -> {
                        answered[0] = true;
                        nativeTextResult(edit.getText().toString(), true);
                    })
                    .setNegativeButton(android.R.string.cancel, (d, w) -> {
                        answered[0] = true;
                        nativeTextResult(initial, false);
                    })
                    .setOnDismissListener(d -> {
                        if (!answered[0]) nativeTextResult(initial, false);
                        mTextDialog = null;
                        enterImmersive();
                    })
                    .create();
            edit.setOnEditorActionListener((tv, actionId, ev) -> {
                // IME "done", or Enter on a hardware keyboard
                boolean enter = actionId == EditorInfo.IME_NULL && ev != null
                        && ev.getKeyCode() == KeyEvent.KEYCODE_ENTER && ev.getAction() == KeyEvent.ACTION_DOWN;
                if (actionId == EditorInfo.IME_ACTION_DONE || enter) {
                    answered[0] = true;
                    nativeTextResult(edit.getText().toString(), true);
                    dlg.dismiss();
                    return true;
                }
                return false;
            });
            mTextDialog = dlg;
            dlg.show();
            edit.requestFocus();
            edit.postDelayed(() -> {
                InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
                if (imm != null) imm.showSoftInput(edit, InputMethodManager.SHOW_IMPLICIT);
            }, 150);
        });
    }

    @SuppressWarnings("unused")
    void cancelTextInput() {
        mMain.post(() -> {
            if (mTextDialog != null) {
                AlertDialog d = mTextDialog;
                mTextDialog = null;
                d.setOnDismissListener(null);
                d.dismiss();
                enterImmersive();
            }
        });
    }

    @SuppressWarnings("unused")
    void showAlert(String title, String msg, String[] buttons) {
        mMain.post(() -> {
            AlertDialog.Builder b = new AlertDialog.Builder(this).setTitle(title).setMessage(msg).setCancelable(false);
            if (buttons.length > 0) b.setPositiveButton(buttons[0], (d, w) -> nativeAlertResult(0));
            if (buttons.length > 1) b.setNegativeButton(buttons[1], (d, w) -> nativeAlertResult(1));
            if (buttons.length > 2) b.setNeutralButton(buttons[2], (d, w) -> nativeAlertResult(2));
            if (buttons.length == 0) b.setPositiveButton(android.R.string.ok, (d, w) -> nativeAlertResult(0));
            b.setOnDismissListener(d -> enterImmersive());
            showDialog(b.create());
        });
    }

    @SuppressWarnings("unused")
    void openUrl(String url) {
        mMain.post(() -> {
            try {
                startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
            } catch (Exception ignored) {
            }
        });
    }

    @SuppressWarnings("unused")
    void showFatal(String msg) {
        mMain.post(() -> new AlertDialog.Builder(this)
                .setTitle(R.string.fatal_title)
                .setMessage(msg)
                .setCancelable(false)
                .setPositiveButton(android.R.string.ok, (d, w) -> finishAndRemoveTask())
                .show());
    }

    @SuppressWarnings("unused")
    void confirmQuit() {
        mMain.post(() -> {
            if (mQuitDialog != null || mExiting) return;
            mQuitDialog = new AlertDialog.Builder(this)
                    .setTitle(R.string.quit_title)
                    .setPositiveButton(R.string.quit_yes, (d, w) -> nativeQuit())
                    .setNegativeButton(android.R.string.cancel, null)
                    .setNeutralButton(R.string.settings_short,
                            (d, w) -> startActivity(new Intent(this, SettingsActivity.class)))
                    .setOnDismissListener(d -> {
                        mQuitDialog = null;
                        enterImmersive();
                    })
                    .create();
            showDialog(mQuitDialog);
        });
    }

    // Light feedback for port gestures: 1 the magnifier appeared, 2 a two-finger tap paused or resumed,
    // 3 a right click for a mod.
    @SuppressWarnings("unused")
    void haptic(int kind) {
        mMain.post(() -> {
            if (mSurface == null) return;
            int fx = kind == 1 ? HapticFeedbackConstants.LONG_PRESS
                    : kind == 3 ? HapticFeedbackConstants.CONTEXT_CLICK
                    : Build.VERSION.SDK_INT >= 30 ? HapticFeedbackConstants.CONFIRM : HapticFeedbackConstants.VIRTUAL_KEY;
            mSurface.performHapticFeedback(fx);
        });
    }

    // The game stopped drawing while on screen (native watchdog). It's been marked
    // as a crash, so the next start offers the logs and skips the saved run.
    @SuppressWarnings("unused")
    void onHang() {
        mMain.post(() -> {
            if (mExiting || isFinishing()) return;
            AlertDialog d = new AlertDialog.Builder(this)
                    .setTitle(R.string.hang_title)
                    .setMessage(R.string.hang_body)
                    .setPositiveButton(R.string.hang_close, (dlg, w) -> {
                        finishAndRemoveTask();
                        android.os.Process.killProcess(android.os.Process.myPid());
                    })
                    .setNegativeButton(R.string.hang_wait, null)
                    .setOnDismissListener(dlg -> enterImmersive())
                    .create();
            showDialog(d);
        });
    }

    // The game crashed last time: say so once, and offer the log.
    private void offerCrashLogs() {
        if (isFinishing()) return;
        AlertDialog.Builder b = new AlertDialog.Builder(this)
                .setTitle(R.string.crash_title)
                .setMessage(R.string.crash_body)
                .setPositiveButton(R.string.set_save_logs, (dlg, w) -> {
                    Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
                    i.addCategory(Intent.CATEGORY_OPENABLE);
                    i.setType("application/zip");
                    i.putExtra(Intent.EXTRA_TITLE, Logs.exportName());
                    startActivityForResult(i, REQ_LOGS);
                })
                .setNegativeButton(android.R.string.cancel, null)
                .setOnDismissListener(dlg -> enterImmersive());
        // A mod is a likely cause: offer to play without.
        if (Mods.activeIndex(this) != null)
            b.setNeutralButton(R.string.crash_mods, (dlg, w) -> {
                Mods.turnOff(this);
                Intent i = new Intent(this, GameActivity.class);
                i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
                startActivity(i);
                finishAffinity();
                android.os.Process.killProcess(android.os.Process.myPid());
            });
        showDialog(b.create());
    }

    private void saveLogs(Uri uri) {
        new Thread(() -> {
            String err = null;
            try {
                Logs.exportTo(this, uri);
            } catch (Exception e) {
                err = e.getMessage();
            }
            final String ferr = err;
            mMain.post(() -> android.widget.Toast.makeText(this, ferr == null ? getString(R.string.logs_saved) : ferr,
                    android.widget.Toast.LENGTH_SHORT).show());
        }, "logs").start();
    }

    // The game saved and called exit(): close the task, then end the process
    // (the native game thread is parked and must not run on).
    @SuppressWarnings("unused")
    void onGameExit() {
        mMain.post(() -> {
            mExiting = true;
            finishAndRemoveTask();
        });
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (mExiting) android.os.Process.killProcess(android.os.Process.myPid());
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
