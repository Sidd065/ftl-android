// Multi-touch injector for testing on a device/emulator (runs as the shell user):
//   adb push inject.dex /data/local/tmp/ && adb shell CLASSPATH=/data/local/tmp/inject.dex \
//       app_process / Inject /data/local/tmp/script.txt
// Script lines: "down <id> <x> <y>", "move <id> <x> <y>", "up <id> <x> <y>", "sleep <ms>",
//               "glide <id> <x0> <y0> <x1> <y1> <ms>" (move one pointer in steps),
//               "pad <keycode> <down|up|tap>" (gamepad button),
//               "axes <lx> <ly> <rx> <ry> <lt> <rt> <hatx> <haty>" (joystick motion).
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.MotionEvent;
import java.io.BufferedReader;
import java.io.FileReader;
import java.lang.reflect.Method;
import java.util.ArrayList;

public class Inject {
    static Object sIm;
    static Method sInject;
    static final ArrayList<float[]> sPtrs = new ArrayList<>();  // {id, x, y}
    static long sDownTime;

    static void send(int action) throws Exception {
        int n = sPtrs.size();
        MotionEvent.PointerProperties[] props = new MotionEvent.PointerProperties[n];
        MotionEvent.PointerCoords[] coords = new MotionEvent.PointerCoords[n];
        for (int i = 0; i < n; i++) {
            props[i] = new MotionEvent.PointerProperties();
            props[i].id = (int) sPtrs.get(i)[0];
            props[i].toolType = MotionEvent.TOOL_TYPE_FINGER;
            coords[i] = new MotionEvent.PointerCoords();
            coords[i].x = sPtrs.get(i)[1];
            coords[i].y = sPtrs.get(i)[2];
            coords[i].pressure = 1;
            coords[i].size = 0.1f;
        }
        long now = SystemClock.uptimeMillis();
        MotionEvent e = MotionEvent.obtain(sDownTime, now, action, n, props, coords, 0, 0, 1, 1, 0, 0,
                InputDevice.SOURCE_TOUCHSCREEN, 0);
        sInject.invoke(sIm, e, 2);  // INJECT_INPUT_EVENT_MODE_WAIT_FOR_FINISH
        e.recycle();
    }

    static void sendKey(long downTime, int action, int code) throws Exception {
        KeyEvent k = new KeyEvent(downTime, SystemClock.uptimeMillis(), action, code, 0, 0,
                KeyCharacterMap.VIRTUAL_KEYBOARD, 0, 0, InputDevice.SOURCE_GAMEPAD);
        sInject.invoke(sIm, k, 2);
    }

    static int indexOf(int id) {
        for (int i = 0; i < sPtrs.size(); i++)
            if ((int) sPtrs.get(i)[0] == id) return i;
        return -1;
    }

    public static void main(String[] args) throws Exception {
        Class<?> c;
        try {  // Android 14+
            c = Class.forName("android.hardware.input.InputManagerGlobal");
        } catch (ClassNotFoundException e) {
            c = Class.forName("android.hardware.input.InputManager");
        }
        sIm = c.getMethod("getInstance").invoke(null);
        sInject = c.getMethod("injectInputEvent", InputEvent.class, int.class);
        BufferedReader r = new BufferedReader(new FileReader(args[0]));
        for (String line; (line = r.readLine()) != null; ) {
            String[] t = line.trim().split("\\s+");
            if (t.length == 0 || t[0].isEmpty() || t[0].startsWith("#")) continue;
            switch (t[0]) {
                case "sleep":
                    Thread.sleep(Long.parseLong(t[1]));
                    break;
                case "down": {
                    float[] p = {Integer.parseInt(t[1]), Float.parseFloat(t[2]), Float.parseFloat(t[3])};
                    sPtrs.add(p);
                    if (sPtrs.size() == 1) {
                        sDownTime = SystemClock.uptimeMillis();
                        send(MotionEvent.ACTION_DOWN);
                    } else {
                        send(MotionEvent.ACTION_POINTER_DOWN
                                | ((sPtrs.size() - 1) << MotionEvent.ACTION_POINTER_INDEX_SHIFT));
                    }
                    break;
                }
                case "move": {
                    int i = indexOf(Integer.parseInt(t[1]));
                    sPtrs.get(i)[1] = Float.parseFloat(t[2]);
                    sPtrs.get(i)[2] = Float.parseFloat(t[3]);
                    send(MotionEvent.ACTION_MOVE);
                    break;
                }
                case "glide": {
                    int i = indexOf(Integer.parseInt(t[1]));
                    float x0 = Float.parseFloat(t[2]), y0 = Float.parseFloat(t[3]);
                    float x1 = Float.parseFloat(t[4]), y1 = Float.parseFloat(t[5]);
                    int ms = Integer.parseInt(t[6]), steps = Math.max(1, ms / 16);
                    for (int k = 1; k <= steps; k++) {
                        sPtrs.get(i)[1] = x0 + (x1 - x0) * k / steps;
                        sPtrs.get(i)[2] = y0 + (y1 - y0) * k / steps;
                        send(MotionEvent.ACTION_MOVE);
                        Thread.sleep(16);
                    }
                    break;
                }
                case "up": {
                    int i = indexOf(Integer.parseInt(t[1]));
                    sPtrs.get(i)[1] = Float.parseFloat(t[2]);
                    sPtrs.get(i)[2] = Float.parseFloat(t[3]);
                    if (sPtrs.size() == 1) send(MotionEvent.ACTION_UP);
                    else send(MotionEvent.ACTION_POINTER_UP | (i << MotionEvent.ACTION_POINTER_INDEX_SHIFT));
                    sPtrs.remove(i);
                    break;
                }
                case "pad": {
                    int code = Integer.parseInt(t[1]);
                    long now = SystemClock.uptimeMillis();
                    if (!t[2].equals("up")) sendKey(now, KeyEvent.ACTION_DOWN, code);
                    if (t[2].equals("tap")) Thread.sleep(60);
                    if (!t[2].equals("down")) sendKey(now, KeyEvent.ACTION_UP, code);
                    break;
                }
                case "axes": {
                    int[] ids = {MotionEvent.AXIS_X, MotionEvent.AXIS_Y, MotionEvent.AXIS_Z, MotionEvent.AXIS_RZ,
                            MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_RTRIGGER, MotionEvent.AXIS_HAT_X,
                            MotionEvent.AXIS_HAT_Y};
                    MotionEvent.PointerProperties[] pp = {new MotionEvent.PointerProperties()};
                    pp[0].id = 0;
                    pp[0].toolType = MotionEvent.TOOL_TYPE_UNKNOWN;
                    MotionEvent.PointerCoords[] pc = {new MotionEvent.PointerCoords()};
                    for (int i = 0; i < ids.length && i + 1 < t.length; i++)
                        pc[0].setAxisValue(ids[i], Float.parseFloat(t[i + 1]));
                    long now = SystemClock.uptimeMillis();
                    MotionEvent e = MotionEvent.obtain(now, now, MotionEvent.ACTION_MOVE, 1, pp, pc, 0, 0, 1, 1, 0, 0,
                            InputDevice.SOURCE_JOYSTICK, 0);
                    sInject.invoke(sIm, e, 2);
                    e.recycle();
                    break;
                }
                default:
                    System.err.println("unknown: " + line);
            }
        }
    }
}
