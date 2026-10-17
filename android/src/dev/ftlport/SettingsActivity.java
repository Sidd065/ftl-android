package dev.ftlport;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicReference;

// Port settings: reachable from the launcher icon's long-press menu, the
// system "additional settings" link, the quit prompt and the import screen.
// Touch options apply when the game resumes; render resolution on the next start.
public class SettingsActivity extends Activity {
    static final String PREFS = "ftlport";
    static final String K_MAGNIFIER = "magnifier";          // int: 0 automatic, 1 always, 2 off
    static final String K_TWO_FINGER = "two_finger_pause";  // boolean
    static final String K_MINIMAP = "minimap";              // boolean
    static final String K_HAPTICS = "haptics";              // boolean
    static final String K_RENDER = "render_height";         // float: 0 automatic (sharp), 768 standard
    static final String K_EVENT_TEXT = "event_text";        // int: 0 normal, 1 large, 2 larger
    static final String K_WINDOW_ZOOM = "window_zoom";      // int: 0 automatic, 1 always, 2 off
    static final String K_EVENT_ZOOM = "event_zoom";        // boolean
    static final String K_RESUME = "resume_run";            // boolean
    static final String K_PAD_MAGNETISM = "pad_magnetism";  // boolean: the cursor is drawn to buttons
    static final String K_PAD_RUMBLE = "pad_rumble";        // boolean
    static final String K_PAD_TOUCHPAD = "pad_touchpad";    // boolean: DualSense / DualShock touchpad moves it

    private static final int REQ_EXPORT = 1, REQ_IMPORT = 2, REQ_LOGS = 3;

    interface IntSink { void put(int v); }
    interface BoolSink { void put(boolean v); }
    interface Job { void run() throws IOException; }

    private LinearLayout mCol;
    private SharedPreferences mPrefs;
    private Button mUndo;
    private TextView mModsDesc;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Logs.installCrashHandler(this);
        mPrefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        mCol = new LinearLayout(this);
        mCol.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(20);
        mCol.setPadding(pad, dp(12), pad, pad);

        TextView title = new TextView(this);
        title.setText(R.string.settings_title);
        title.setTextSize(22);
        title.setTextColor(0xFFECEFF1);
        title.setPadding(0, dp(8), 0, dp(4));
        mCol.addView(title);

        section(R.string.set_touch);
        choice(R.string.set_magnifier, R.string.set_magnifier_desc,
                new int[] {R.string.set_mag_auto, R.string.set_mag_on, R.string.set_mag_off},
                mPrefs.getInt(K_MAGNIFIER, 0), v -> mPrefs.edit().putInt(K_MAGNIFIER, v).apply());
        toggle(R.string.set_two_finger, R.string.set_two_finger_desc, mPrefs.getBoolean(K_TWO_FINGER, true),
                v -> mPrefs.edit().putBoolean(K_TWO_FINGER, v).apply());
        toggle(R.string.set_minimap, R.string.set_minimap_desc, mPrefs.getBoolean(K_MINIMAP, true),
                v -> mPrefs.edit().putBoolean(K_MINIMAP, v).apply());
        toggle(R.string.set_haptics, R.string.set_haptics_desc, mPrefs.getBoolean(K_HAPTICS, true),
                v -> mPrefs.edit().putBoolean(K_HAPTICS, v).apply());

        section(R.string.set_play);
        toggle(R.string.set_resume, R.string.set_resume_desc, mPrefs.getBoolean(K_RESUME, false),
                v -> mPrefs.edit().putBoolean(K_RESUME, v).apply());

        section(R.string.set_graphics);
        choice(R.string.set_window_zoom, R.string.set_window_zoom_desc,
                new int[] {R.string.set_mag_auto, R.string.set_mag_on, R.string.set_mag_off},
                mPrefs.getInt(K_WINDOW_ZOOM, 0), v -> mPrefs.edit().putInt(K_WINDOW_ZOOM, v).apply());
        toggle(R.string.set_event_zoom, R.string.set_event_zoom_desc, mPrefs.getBoolean(K_EVENT_ZOOM, false),
                v -> mPrefs.edit().putBoolean(K_EVENT_ZOOM, v).apply());
        choice(R.string.set_event_text, R.string.set_event_text_desc,
                new int[] {R.string.set_event_text_normal, R.string.set_event_text_large,
                        R.string.set_event_text_larger},
                mPrefs.getInt(K_EVENT_TEXT, 0), v -> mPrefs.edit().putInt(K_EVENT_TEXT, v).apply());
        choice(R.string.set_render, R.string.set_render_desc,
                new int[] {R.string.set_render_sharp, R.string.set_render_fast},
                mPrefs.getFloat(K_RENDER, 0) == 768 ? 1 : 0,
                v -> mPrefs.edit().putFloat(K_RENDER, v == 1 ? 768 : 0).apply());

        section(R.string.set_controls);
        text(R.string.set_controls_body, 15, 0xFFCFD8DC);

        section(R.string.set_controller);
        text(R.string.set_controller_desc, 14, 0xFF90A4AE);
        button(R.string.set_pad_map, v -> startActivity(new Intent(this, ControllerActivity.class)));
        toggle(R.string.set_pad_magnetism, R.string.set_pad_magnetism_desc, mPrefs.getBoolean(K_PAD_MAGNETISM, true),
                v -> mPrefs.edit().putBoolean(K_PAD_MAGNETISM, v).apply());
        toggle(R.string.set_pad_rumble, R.string.set_pad_rumble_desc, mPrefs.getBoolean(K_PAD_RUMBLE, true),
                v -> mPrefs.edit().putBoolean(K_PAD_RUMBLE, v).apply());
        toggle(R.string.set_pad_touchpad, R.string.set_pad_touchpad_desc, mPrefs.getBoolean(K_PAD_TOUCHPAD, true),
                v -> mPrefs.edit().putBoolean(K_PAD_TOUCHPAD, v).apply());

        section(R.string.set_saves);
        text(R.string.set_saves_desc, 14, 0xFF90A4AE);
        button(R.string.set_export, v -> exportSaves());
        button(R.string.set_import, v -> pick(REQ_IMPORT));
        mUndo = button(R.string.set_undo_import, v -> confirmUndo());

        section(R.string.set_mods);
        mModsDesc = text(R.string.set_mods_desc, 14, 0xFF90A4AE);
        button(R.string.set_mods_button, v -> startActivity(new Intent(this, ModsActivity.class)));

        section(R.string.set_files);
        text(R.string.set_files_desc, 14, 0xFF90A4AE);
        button(R.string.set_reimport, v -> confirmReimport());

        section(R.string.set_logs);
        text(R.string.set_logs_desc, 14, 0xFF90A4AE);
        button(R.string.set_save_logs, v -> create(REQ_LOGS, Logs.exportName()));

        ScrollView sv = new ScrollView(this);
        // Android 15+ lays apps out edge to edge: keep the content clear of the system bars.
        sv.setFitsSystemWindows(true);
        sv.addView(mCol);
        setContentView(sv);
    }

    @Override
    protected void onResume() {
        super.onResume();
        mUndo.setVisibility(SaveTransfer.canUndo(this) ? View.VISIBLE : View.GONE);
        int mods = Mods.applied(this).size();
        mModsDesc.setText(mods > 0 ? getResources().getQuantityString(R.plurals.mods_status_on, mods, mods)
                : getString(R.string.set_mods_desc));
    }

    // Only builds made with Hyperspace (android/build-apk.sh HYPERSPACE_SO=...) carry it.
    static File hyperspaceLib(android.content.Context ctx) {
        File f = new File(ctx.getApplicationInfo().nativeLibraryDir, "libHyperspace.so");
        return f.isFile() ? f : null;
    }

    private void section(int title) {
        TextView t = new TextView(this);
        t.setText(title);
        t.setTextSize(14);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setTextColor(0xFF80CBC4);
        t.setPadding(0, dp(20), 0, dp(4));
        mCol.addView(t);
    }

    private TextView text(int res, int sp, int color) {
        TextView t = new TextView(this);
        t.setText(res);
        t.setTextSize(sp);
        t.setTextColor(color);
        t.setLineSpacing(0, 1.15f);
        mCol.addView(t);
        return t;
    }

    private Button button(int label, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setOnClickListener(l);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(8);
        mCol.addView(b, lp);
        return b;
    }

    private void toggle(int title, int desc, boolean value, BoolSink sink) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(8), 0, dp(8));
        LinearLayout texts = new LinearLayout(this);
        texts.setOrientation(LinearLayout.VERTICAL);
        TextView t = new TextView(this);
        t.setText(title);
        t.setTextSize(17);
        t.setTextColor(0xFFECEFF1);
        TextView d = new TextView(this);
        d.setText(desc);
        d.setTextSize(14);
        d.setTextColor(0xFF90A4AE);
        texts.addView(t);
        texts.addView(d);
        row.addView(texts, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        Switch sw = new Switch(this);
        sw.setChecked(value);
        sw.setOnCheckedChangeListener((b, on) -> sink.put(on));
        row.addView(sw);
        row.setOnClickListener(v -> sw.toggle());
        mCol.addView(row);
    }

    private void choice(int title, int desc, int[] options, int value, IntSink sink) {
        TextView t = new TextView(this);
        t.setText(title);
        t.setTextSize(17);
        t.setTextColor(0xFFECEFF1);
        t.setPadding(0, dp(8), 0, 0);
        mCol.addView(t);
        TextView d = new TextView(this);
        d.setText(desc);
        d.setTextSize(14);
        d.setTextColor(0xFF90A4AE);
        mCol.addView(d);
        RadioGroup g = new RadioGroup(this);
        g.setOrientation(RadioGroup.VERTICAL);
        for (int i = 0; i < options.length; i++) {
            RadioButton rb = new RadioButton(this);
            rb.setId(View.generateViewId());
            rb.setText(options[i]);
            rb.setTextSize(15);
            rb.setTextColor(0xFFCFD8DC);
            final int idx = i;
            rb.setOnCheckedChangeListener((b, on) -> {
                if (on) sink.put(idx);
            });
            g.addView(rb);
            if (i == value) rb.setChecked(true);
        }
        mCol.addView(g);
    }

    // ------------------------------------------------------------------ saves and logs
    private void exportSaves() {
        if (!SaveTransfer.hasSaves(this)) {
            Toast.makeText(this, R.string.saves_nothing, Toast.LENGTH_SHORT).show();
            return;
        }
        create(REQ_EXPORT, SaveTransfer.exportName());
    }

    private void create(int req, String name) {
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("application/zip");
        i.putExtra(Intent.EXTRA_TITLE, name);
        startActivityForResult(i, req);
    }

    private void pick(int req) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        startActivityForResult(i, req);
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (res != RESULT_OK || data == null) return;
        Uri uri = data.getData();
        if (req == REQ_EXPORT && uri != null) {
            background(() -> SaveTransfer.exportTo(this, uri), () -> toast(R.string.saves_exported));
        } else if (req == REQ_LOGS && uri != null) {
            background(() -> Logs.exportTo(this, uri), () -> toast(R.string.logs_saved));
        } else if (req == REQ_IMPORT) {
            List<Uri> uris = new ArrayList<>();
            ClipData clip = data.getClipData();
            if (clip != null)
                for (int i = 0; i < clip.getItemCount(); i++) uris.add(clip.getItemAt(i).getUri());
            else if (uri != null) uris.add(uri);
            AtomicReference<Map<String, byte[]>> files = new AtomicReference<>();
            background(() -> files.set(SaveTransfer.read(this, uris)), () -> confirmImport(files.get()));
        }
    }

    private void confirmImport(Map<String, byte[]> files) {
        new AlertDialog.Builder(this)
                .setTitle(R.string.saves_confirm_title)
                .setMessage(getString(R.string.saves_confirm, SaveTransfer.describe(this, files)))
                .setPositiveButton(R.string.saves_replace,
                        (d, w) -> background(() -> SaveTransfer.install(this, files), this::restart))
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    private void confirmUndo() {
        new AlertDialog.Builder(this)
                .setTitle(R.string.set_undo_import)
                .setMessage(R.string.saves_undo_confirm)
                .setPositiveButton(R.string.saves_undo_yes,
                        (d, w) -> background(() -> SaveTransfer.undoImport(this), this::restart))
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    // File work off the UI thread; errors are shown to the player.
    private void background(Job job, Runnable then) {
        new Thread(() -> {
            String err = null;
            try {
                job.run();
            } catch (IOException | RuntimeException e) {
                err = e.getMessage() != null ? e.getMessage() : e.toString();
            }
            final String ferr = err;
            runOnUiThread(() -> {
                if (isFinishing()) return;
                if (ferr == null) then.run();
                else new AlertDialog.Builder(this).setMessage(ferr).setPositiveButton(android.R.string.ok, null).show();
            });
        }, "settings-io").start();
    }

    private void toast(int msg) {
        Toast.makeText(this, msg, Toast.LENGTH_SHORT).show();
    }

    // ------------------------------------------------------------------ game files
    private void confirmReimport() {
        new AlertDialog.Builder(this)
                .setTitle(R.string.set_reimport)
                .setMessage(R.string.set_reimport_confirm)
                .setPositiveButton(R.string.set_reimport_yes, (dlg, w) -> {
                    deleteTree(Importer.gameDir(this));
                    restart();
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    // The game may be running in this process with the old files loaded, so
    // start over (into the importer if the game files are gone).
    private void restart() {
        Intent i = new Intent(this, GameActivity.class);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(i);
        finishAffinity();
        android.os.Process.killProcess(android.os.Process.myPid());
    }

    private static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null)
            for (File k : kids) deleteTree(k);
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
