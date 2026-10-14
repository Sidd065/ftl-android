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
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.util.DisplayMetrics;
import android.view.Display;
import android.view.DisplayCutout;
import android.view.Gravity;
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
    private static final int REQ_ARCHIVE = 1, REQ_TREE = 2;
    private static boolean sNativeStarted;  // the game runs once per process

    private SurfaceView mSurface;
    private boolean mLibLoaded;
    private AlertDialog mTextDialog;
    private AlertDialog mQuitDialog;
    private boolean mExiting;
    private final Handler mMain = new Handler(Looper.getMainLooper());

    // ---- native ----
    private native void nativeStart(String bin, String bundle, String docs, String cache, int sw, int sh, int refresh,
                                    float[] layout);
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

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(lp);
        }
        getWindow().getDecorView().setBackgroundColor(Color.BLACK);
        if (Importer.isInstalled(this)) startGame();
        else tryAutoImportThenShowImporter();
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
        runImport(req == REQ_TREE, uri);
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
            File docs = new File(getFilesDir(), "Documents");
            File cache = new File(getCacheDir(), "ftl");
            //noinspection ResultOfMethodCallIgnored
            docs.mkdirs();
            //noinspection ResultOfMethodCallIgnored
            cache.mkdirs();
            nativeStart(new File(game, "FTL").getPath(), game.getPath(), docs.getPath(), cache.getPath(), w, h,
                    refreshRate(), computeLayout(w, h));
            pushOptions();
            scanPads();
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
        if (mSurface != null && sNativeStarted && e.isFromSource(android.view.InputDevice.SOURCE_MOUSE))
            return onMouse(e);
        if (mSurface != null && sNativeStarted && e.isFromSource(android.view.InputDevice.SOURCE_JOYSTICK)
                && e.getActionMasked() == MotionEvent.ACTION_MOVE) {
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

    private static boolean isPadEvent(KeyEvent e) {
        int code = e.getKeyCode();
        if (KeyEvent.isGamepadButton(code)) return true;
        boolean dpad = code == KeyEvent.KEYCODE_DPAD_UP || code == KeyEvent.KEYCODE_DPAD_DOWN
                || code == KeyEvent.KEYCODE_DPAD_LEFT || code == KeyEvent.KEYCODE_DPAD_RIGHT;
        return dpad && (e.isFromSource(android.view.InputDevice.SOURCE_GAMEPAD)
                || e.isFromSource(android.view.InputDevice.SOURCE_JOYSTICK));
    }

    // Gamepads reach the game through its own (iOS) controller support.
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
            int src = d.getSources();
            if ((src & android.view.InputDevice.SOURCE_GAMEPAD) == android.view.InputDevice.SOURCE_GAMEPAD
                    || (src & android.view.InputDevice.SOURCE_JOYSTICK) == android.view.InputDevice.SOURCE_JOYSTICK) {
                name = d.getName();
                break;
            }
        }
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
        if (e.getToolType(0) == MotionEvent.TOOL_TYPE_MOUSE) return onMouse(e);
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
        if (mSurface != null && sNativeStarted) {
            pushOptions();
            nativeResume();
            enterImmersive();
            android.hardware.input.InputManager im = getSystemService(android.hardware.input.InputManager.class);
            if (im != null) im.registerInputDeviceListener(mPadListener, mMain);
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
                    .setOnDismissListener(d -> {
                        mQuitDialog = null;
                        enterImmersive();
                    })
                    .create();
            showDialog(mQuitDialog);
        });
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
