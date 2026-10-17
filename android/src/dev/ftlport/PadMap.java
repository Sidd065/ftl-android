package dev.ftlport;

import android.content.SharedPreferences;
import android.view.KeyEvent;

import java.util.Map;
import java.util.TreeMap;

/** Which game controller button does what: the defaults, and the player's own in settings. */
final class PadMap {
    // Actions: the numbers are pad.c's
    static final int NONE = 0, TAP = 1, RIGHT_CLICK = 2, BACK = 3, PAUSE = 4, ZOOM_IN = 5, ZOOM_OUT = 6,
            RESET_ZOOM = 7, MAGNIFIER = 8, TURBO = 9, PRECISION = 10;
    // In the order the settings list them
    static final int[] ACTIONS = {TAP, RIGHT_CLICK, BACK, PAUSE, ZOOM_IN, ZOOM_OUT, RESET_ZOOM, MAGNIFIER, TURBO,
            PRECISION};
    static final int[][] NAMES = {
            {TAP, R.string.pad_act_tap, R.string.pad_act_tap_desc},
            {RIGHT_CLICK, R.string.pad_act_right, R.string.pad_act_right_desc},
            {BACK, R.string.pad_act_back, R.string.pad_act_back_desc},
            {PAUSE, R.string.pad_act_pause, 0},
            {ZOOM_IN, R.string.pad_act_zoom_in, R.string.pad_act_zoom_desc},
            {ZOOM_OUT, R.string.pad_act_zoom_out, R.string.pad_act_zoom_desc},
            {RESET_ZOOM, R.string.pad_act_reset_zoom, 0},
            {MAGNIFIER, R.string.pad_act_magnifier, R.string.pad_act_held_desc},
            {TURBO, R.string.pad_act_turbo, R.string.pad_act_turbo_desc},
            {PRECISION, R.string.pad_act_precision, R.string.pad_act_precision_desc},
    };

    private static final String PREF = "pad_map";  // "keycode:action,..."; missing: the defaults

    private PadMap() {}

    /** Button (Android key code) -> action */
    static Map<Integer, Integer> defaults() {
        Map<Integer, Integer> m = new TreeMap<>();
        m.put(KeyEvent.KEYCODE_BUTTON_A, TAP);
        m.put(KeyEvent.KEYCODE_BUTTON_B, BACK);
        m.put(KeyEvent.KEYCODE_BUTTON_START, BACK);
        m.put(KeyEvent.KEYCODE_BUTTON_X, PAUSE);
        m.put(KeyEvent.KEYCODE_BUTTON_L1, ZOOM_OUT);
        m.put(KeyEvent.KEYCODE_BUTTON_R1, ZOOM_IN);
        m.put(KeyEvent.KEYCODE_BUTTON_L2, MAGNIFIER);
        m.put(KeyEvent.KEYCODE_BUTTON_R2, RIGHT_CLICK);
        m.put(KeyEvent.KEYCODE_BUTTON_SELECT, RESET_ZOOM);
        m.put(KeyEvent.KEYCODE_BUTTON_THUMBR, RESET_ZOOM);
        return m;
    }

    static Map<Integer, Integer> load(SharedPreferences prefs) {
        String s = prefs.getString(PREF, null);
        if (s == null) return defaults();
        Map<Integer, Integer> m = new TreeMap<>();
        for (String pair : s.split(",")) {
            String[] kv = pair.split(":");
            if (kv.length != 2) continue;
            try {
                int key = Integer.parseInt(kv[0]), act = Integer.parseInt(kv[1]);
                if (key > 0 && key < 256 && act > NONE && act <= PRECISION) m.put(key, act);
            } catch (NumberFormatException ignored) {
            }
        }
        return m;
    }

    static void save(SharedPreferences prefs, Map<Integer, Integer> m) {
        StringBuilder b = new StringBuilder();
        for (Map.Entry<Integer, Integer> e : m.entrySet()) {
            if (b.length() > 0) b.append(',');
            b.append(e.getKey()).append(':').append(e.getValue());
        }
        prefs.edit().putString(PREF, b.toString()).apply();
    }

    static void reset(SharedPreferences prefs) {
        prefs.edit().remove(PREF).apply();
    }

    /** A button that can be mapped: any gamepad button but the d-pad, which moves the cursor. */
    static boolean mappable(int keyCode) {
        return keyCode > 0 && keyCode < 256 && KeyEvent.isGamepadButton(keyCode);
    }

    static String buttonName(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_BUTTON_A: return "A";
            case KeyEvent.KEYCODE_BUTTON_B: return "B";
            case KeyEvent.KEYCODE_BUTTON_C: return "C";
            case KeyEvent.KEYCODE_BUTTON_X: return "X";
            case KeyEvent.KEYCODE_BUTTON_Y: return "Y";
            case KeyEvent.KEYCODE_BUTTON_Z: return "Z";
            case KeyEvent.KEYCODE_BUTTON_L1: return "LB";
            case KeyEvent.KEYCODE_BUTTON_R1: return "RB";
            case KeyEvent.KEYCODE_BUTTON_L2: return "LT";
            case KeyEvent.KEYCODE_BUTTON_R2: return "RT";
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return "L3";
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return "R3";
            case KeyEvent.KEYCODE_BUTTON_START: return "Start";
            case KeyEvent.KEYCODE_BUTTON_SELECT: return "Select";
            case KeyEvent.KEYCODE_BUTTON_MODE: return "Home";
            default:
                if (keyCode >= KeyEvent.KEYCODE_BUTTON_1 && keyCode <= KeyEvent.KEYCODE_BUTTON_16)
                    return "Button " + (keyCode - KeyEvent.KEYCODE_BUTTON_1 + 1);
                return KeyEvent.keyCodeToString(keyCode).replace("KEYCODE_", "");
        }
    }
}
