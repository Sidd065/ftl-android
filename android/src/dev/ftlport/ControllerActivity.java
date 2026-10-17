package dev.ftlport;

import android.app.Activity;
import android.content.SharedPreferences;
import android.graphics.Typeface;
import android.hardware.input.InputManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/** Settings: which game controller button does what. Tap an action, then press a button for it. */
public class ControllerActivity extends Activity {
    private SharedPreferences mPrefs;
    private Map<Integer, Integer> mMap;
    private LinearLayout mList;
    private TextView mStatus;
    private View mPrompt;          // "press a button for ..." over the list
    private TextView mPromptText;
    private int mListening = PadMap.NONE;  // the action waiting for a button
    private boolean mTriggerDown;  // a trigger pressed while waiting: ignore it until let go
    private final Handler mMain = new Handler(Looper.getMainLooper());

    private final InputManager.InputDeviceListener mDevices = new InputManager.InputDeviceListener() {
        @Override public void onInputDeviceAdded(int id) { showStatus(); }
        @Override public void onInputDeviceRemoved(int id) { showStatus(); }
        @Override public void onInputDeviceChanged(int id) { showStatus(); }
    };

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Logs.installCrashHandler(this);
        mPrefs = getSharedPreferences(SettingsActivity.PREFS, MODE_PRIVATE);
        mMap = PadMap.load(mPrefs);

        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(20);
        col.setPadding(pad, dp(12), pad, pad);
        TextView title = new TextView(this);
        title.setText(R.string.pad_title);
        title.setTextSize(22);
        title.setTextColor(0xFFECEFF1);
        title.setPadding(0, dp(8), 0, dp(4));
        col.addView(title);
        TextView intro = new TextView(this);
        intro.setText(R.string.pad_intro);
        intro.setTextSize(14);
        intro.setTextColor(0xFF90A4AE);
        intro.setLineSpacing(0, 1.15f);
        col.addView(intro);
        mStatus = new TextView(this);
        mStatus.setTextSize(14);
        mStatus.setTextColor(0xFF80CBC4);
        mStatus.setPadding(0, dp(8), 0, dp(4));
        col.addView(mStatus);
        mList = new LinearLayout(this);
        mList.setOrientation(LinearLayout.VERTICAL);
        col.addView(mList);
        Button reset = new Button(this);
        reset.setText(R.string.pad_reset);
        reset.setAllCaps(false);
        reset.setOnClickListener(v -> {
            PadMap.reset(mPrefs);
            mMap = PadMap.defaults();
            fillList();
        });
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(16);
        col.addView(reset, lp);

        ScrollView sv = new ScrollView(this);
        sv.setFitsSystemWindows(true);
        sv.addView(col);
        FrameLayout root = new FrameLayout(this);
        root.addView(sv);
        root.addView(buildPrompt(), new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));
        setContentView(root);
        fillList();
    }

    private View buildPrompt() {
        LinearLayout p = new LinearLayout(this);
        p.setOrientation(LinearLayout.VERTICAL);
        p.setGravity(Gravity.CENTER);
        p.setBackgroundColor(0xE6101820);
        p.setClickable(true);  // the list underneath stays out of reach
        p.setPadding(dp(24), dp(24), dp(24), dp(24));
        mPromptText = new TextView(this);
        mPromptText.setTextSize(20);
        mPromptText.setTextColor(0xFFECEFF1);
        mPromptText.setGravity(Gravity.CENTER);
        p.addView(mPromptText);
        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setGravity(Gravity.CENTER);
        Button clear = new Button(this);
        clear.setText(R.string.pad_clear);
        clear.setAllCaps(false);
        clear.setOnClickListener(v -> {
            mMap.values().removeIf(a -> a == mListening);
            PadMap.save(mPrefs, mMap);
            stopListening();
        });
        Button cancel = new Button(this);
        cancel.setText(android.R.string.cancel);
        cancel.setAllCaps(false);
        cancel.setOnClickListener(v -> stopListening());
        buttons.addView(clear);
        buttons.addView(cancel);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(20);
        p.addView(buttons, lp);
        p.setVisibility(View.GONE);
        mPrompt = p;
        return p;
    }

    private void fillList() {
        mList.removeAllViews();
        for (int[] a : PadMap.NAMES) {
            final int act = a[0];
            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setPadding(0, dp(10), 0, dp(10));
            row.setFocusable(true);
            row.setBackgroundResource(android.R.drawable.list_selector_background);
            LinearLayout texts = new LinearLayout(this);
            texts.setOrientation(LinearLayout.VERTICAL);
            TextView name = new TextView(this);
            name.setText(a[1]);
            name.setTextSize(17);
            name.setTextColor(0xFFECEFF1);
            texts.addView(name);
            if (a[2] != 0) {
                TextView desc = new TextView(this);
                desc.setText(a[2]);
                desc.setTextSize(14);
                desc.setTextColor(0xFF90A4AE);
                texts.addView(desc);
            }
            row.addView(texts, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
            TextView value = new TextView(this);
            value.setText(buttonsFor(act));
            value.setTextSize(16);
            value.setTypeface(Typeface.DEFAULT_BOLD);
            value.setTextColor(0xFFFFD54F);
            value.setPadding(dp(12), 0, 0, 0);
            row.addView(value);
            row.setOnClickListener(v -> listen(act, a[1]));
            mList.addView(row);
        }
    }

    private String buttonsFor(int act) {
        List<String> names = new ArrayList<>();
        for (Map.Entry<Integer, Integer> e : mMap.entrySet())
            if (e.getValue() == act) names.add(PadMap.buttonName(e.getKey()));
        return names.isEmpty() ? getString(R.string.pad_none) : String.join(", ", names);
    }

    private void listen(int act, int name) {
        mListening = act;
        mTriggerDown = false;
        mPromptText.setText(getString(R.string.pad_listen, getString(name)));
        mPrompt.setVisibility(View.VISIBLE);
    }

    private void stopListening() {
        mListening = PadMap.NONE;
        mPrompt.setVisibility(View.GONE);
        fillList();
    }

    private void assign(int keyCode) {
        mMap.remove(keyCode);  // a button does one thing
        mMap.put(keyCode, mListening);
        PadMap.save(mPrefs, mMap);
        stopListening();
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        if (mListening != PadMap.NONE) {
            int code = e.getKeyCode();
            if (code == KeyEvent.KEYCODE_BACK) {
                if (e.getAction() == KeyEvent.ACTION_UP) stopListening();
                return true;
            }
            if (PadMap.mappable(code)) {
                if (e.getAction() == KeyEvent.ACTION_DOWN && e.getRepeatCount() == 0) assign(code);
                return true;
            }
        }
        return super.dispatchKeyEvent(e);
    }

    // Triggers that only report how far they're pulled
    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if (mListening != PadMap.NONE && e.isFromSource(InputDevice.SOURCE_JOYSTICK)) {
            float lt = Math.max(e.getAxisValue(MotionEvent.AXIS_LTRIGGER), e.getAxisValue(MotionEvent.AXIS_BRAKE));
            float rt = Math.max(e.getAxisValue(MotionEvent.AXIS_RTRIGGER), e.getAxisValue(MotionEvent.AXIS_GAS));
            if (!mTriggerDown && (lt > 0.6f || rt > 0.6f)) {
                mTriggerDown = true;
                assign(rt > lt ? KeyEvent.KEYCODE_BUTTON_R2 : KeyEvent.KEYCODE_BUTTON_L2);
            } else if (lt < 0.2f && rt < 0.2f) {
                mTriggerDown = false;
            }
            return true;
        }
        return super.dispatchGenericMotionEvent(e);
    }

    @Override
    protected void onResume() {
        super.onResume();
        InputManager im = getSystemService(InputManager.class);
        if (im != null) im.registerInputDeviceListener(mDevices, mMain);
        showStatus();
    }

    @Override
    protected void onPause() {
        super.onPause();
        InputManager im = getSystemService(InputManager.class);
        if (im != null) im.unregisterInputDeviceListener(mDevices);
    }

    private void showStatus() {
        List<String> pads = new ArrayList<>();
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d != null && !d.isVirtual() && GameActivity.isGamepad(d)) pads.add(d.getName());
        }
        mStatus.setText(pads.isEmpty() ? getString(R.string.pad_no_controller)
                : getString(R.string.pad_connected, String.join(", ", pads)));
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
