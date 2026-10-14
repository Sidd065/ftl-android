package dev.ftlport;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Typeface;
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

import java.io.File;

// Port settings: reachable from the launcher icon's long-press menu, the
// system "additional settings" link, and the import screen. Touch options
// apply when the game resumes; render resolution on the next start.
public class SettingsActivity extends Activity {
    static final String PREFS = "ftlport";
    static final String K_MAGNIFIER = "magnifier";          // int: 0 automatic, 1 always, 2 off
    static final String K_TWO_FINGER = "two_finger_pause";  // boolean
    static final String K_MINIMAP = "minimap";              // boolean
    static final String K_RENDER = "render_height";         // float: 0 automatic (sharp), 768 standard

    interface IntSink { void put(int v); }
    interface BoolSink { void put(boolean v); }

    private LinearLayout mCol;
    private SharedPreferences mPrefs;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
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

        section(R.string.set_graphics);
        choice(R.string.set_render, R.string.set_render_desc,
                new int[] {R.string.set_render_sharp, R.string.set_render_fast},
                mPrefs.getFloat(K_RENDER, 0) == 768 ? 1 : 0,
                v -> mPrefs.edit().putFloat(K_RENDER, v == 1 ? 768 : 0).apply());

        section(R.string.set_controls);
        text(R.string.set_controls_body, 15, 0xFFCFD8DC);

        section(R.string.set_files);
        text(R.string.set_files_desc, 14, 0xFF90A4AE);
        Button reimport = new Button(this);
        reimport.setText(R.string.set_reimport);
        reimport.setAllCaps(false);
        reimport.setOnClickListener(v -> confirmReimport());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(8);
        mCol.addView(reimport, lp);

        ScrollView sv = new ScrollView(this);
        // Android 15+ lays apps out edge to edge: keep the content clear of the system bars.
        sv.setFitsSystemWindows(true);
        sv.addView(mCol);
        setContentView(sv);
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

    private void confirmReimport() {
        new AlertDialog.Builder(this)
                .setTitle(R.string.set_reimport)
                .setMessage(R.string.set_reimport_confirm)
                .setPositiveButton(R.string.set_reimport_yes, (dlg, w) -> reimport())
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    // Remove the imported copy (saves in Documents are kept) and restart into
    // the importer. The game may be running in this process, so start fresh.
    private void reimport() {
        deleteTree(Importer.gameDir(this));
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
