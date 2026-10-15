package dev.ftlport;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.Intent;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.ImageButton;
import android.widget.LinearLayout;
import android.widget.PopupMenu;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

/**
 * Adding, ordering and switching on mods, then applying them (see Mods).
 * Also where "Open with FTL" (and sharing a file to FTL) lands: a downloaded
 * mod goes straight into the list, ready to apply.
 */
public class ModsActivity extends Activity {
    private static final int REQ_ADD = 1;

    private LinearLayout mList;
    private TextView mStatus;
    private Button mApply, mAdd, mOff;
    private ProgressBar mProgress;
    private List<Mods.Mod> mMods;
    private boolean mBusy;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Logs.installCrashHandler(this);
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(20);
        col.setPadding(pad, dp(12), pad, pad);

        TextView title = new TextView(this);
        title.setText(R.string.mods_title);
        title.setTextSize(22);
        title.setTextColor(0xFFECEFF1);
        title.setPadding(0, dp(8), 0, dp(4));
        col.addView(title);
        TextView about = new TextView(this);
        about.setText(getString(R.string.mods_about) + "\n\n" + getString(
                SettingsActivity.hyperspaceLib(this) != null ? R.string.mods_about_hs : R.string.mods_about_no_hs));
        about.setTextSize(14);
        about.setTextColor(0xFF90A4AE);
        about.setLineSpacing(0, 1.15f);
        col.addView(about);

        mStatus = new TextView(this);
        mStatus.setTextSize(15);
        mStatus.setTextColor(0xFF80CBC4);
        mStatus.setTypeface(Typeface.DEFAULT_BOLD);
        mStatus.setPadding(0, dp(16), 0, dp(4));
        col.addView(mStatus);

        mList = new LinearLayout(this);
        mList.setOrientation(LinearLayout.VERTICAL);
        col.addView(mList);

        mProgress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        mProgress.setMax(1000);
        mProgress.setVisibility(View.GONE);
        col.addView(mProgress);

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setPadding(0, dp(8), 0, 0);
        mAdd = button(buttons, R.string.mods_add, v -> pick());
        mApply = button(buttons, R.string.mods_apply, v -> checkRun(this::apply));
        mOff = button(buttons, R.string.mods_turn_off, v -> confirmOff());
        col.addView(buttons);

        ScrollView sv = new ScrollView(this);
        sv.setFitsSystemWindows(true);
        sv.addView(col);
        setContentView(sv);
        refresh();
        if (state == null) handleIncoming(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        handleIncoming(intent);
    }

    // A file opened with FTL (VIEW) or shared to it (SEND).
    private void handleIncoming(Intent intent) {
        Uri uri = null;
        if (Intent.ACTION_VIEW.equals(intent.getAction())) uri = intent.getData();
        else if (Intent.ACTION_SEND.equals(intent.getAction())) uri = intent.getParcelableExtra(Intent.EXTRA_STREAM);
        if (uri == null) return;
        if (Mods.isIpa(this, uri)) importGame(uri);
        else add(Collections.singletonList(uri));
    }

    // The game itself rather than a mod: hand it to the import screen.
    private void importGame(Uri uri) {
        if (Importer.isInstalled(this)) {
            error(getString(R.string.mods_game_already));
            return;
        }
        Intent i = new Intent(Intent.ACTION_VIEW, uri, this, GameActivity.class);
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(i);
        finish();
    }

    private Button button(LinearLayout row, int label, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setOnClickListener(l);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.rightMargin = dp(8);
        row.addView(b, lp);
        return b;
    }

    private void refresh() {
        mMods = Mods.load(this);
        mList.removeAllViews();
        if (mMods.isEmpty()) {
            TextView none = new TextView(this);
            none.setText(R.string.mods_none);
            none.setTextSize(15);
            none.setTextColor(0xFFCFD8DC);
            none.setPadding(0, dp(8), 0, dp(8));
            mList.addView(none);
        }
        for (int i = 0; i < mMods.size(); i++) mList.addView(row(i));
        List<String> applied = Mods.applied(this);
        boolean pending = !applied.equals(wanted());
        if (applied.isEmpty()) mStatus.setText(pending ? R.string.mods_status_pending : R.string.mods_status_off);
        else mStatus.setText(getResources().getQuantityString(
                pending ? R.plurals.mods_status_on_pending : R.plurals.mods_status_on, applied.size(), applied.size()));
        mApply.setEnabled(!mBusy && pending);
        mOff.setEnabled(!mBusy && !applied.isEmpty());
        mAdd.setEnabled(!mBusy);
    }

    private List<String> wanted() {
        List<String> out = new ArrayList<>();
        for (Mods.Mod m : mMods)
            if (m.on) out.add(m.file);
        return out;
    }

    private View row(int i) {
        Mods.Mod m = mMods.get(i);
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        CheckBox cb = new CheckBox(this);
        cb.setChecked(m.on);
        cb.setText(String.format(Locale.ROOT, "%s  (%s)", m.file, size(Mods.size(this, m.file))));
        cb.setTextSize(16);
        cb.setTextColor(0xFFECEFF1);
        cb.setEnabled(!mBusy);
        cb.setOnCheckedChangeListener((b, on) -> {
            m.on = on;
            saveAndRefresh();
        });
        row.addView(cb, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        ImageButton more = new ImageButton(this);
        more.setImageResource(android.R.drawable.ic_menu_more);
        more.setBackgroundColor(0);
        more.setContentDescription(getString(R.string.mods_more));
        more.setEnabled(!mBusy);
        more.setOnClickListener(v -> menu(v, i));
        row.addView(more);
        return row;
    }

    private void menu(View anchor, int i) {
        PopupMenu pm = new PopupMenu(this, anchor);
        if (i > 0) pm.getMenu().add(0, 1, 0, R.string.mods_up);
        if (i < mMods.size() - 1) pm.getMenu().add(0, 2, 0, R.string.mods_down);
        pm.getMenu().add(0, 3, 0, R.string.mods_remove);
        pm.setOnMenuItemClickListener(item -> {
            switch (item.getItemId()) {
                case 1:
                    mMods.add(i - 1, mMods.remove(i));
                    saveAndRefresh();
                    break;
                case 2:
                    mMods.add(i + 1, mMods.remove(i));
                    saveAndRefresh();
                    break;
                default:
                    confirmRemove(mMods.get(i).file);
            }
            return true;
        });
        pm.show();
    }

    private void saveAndRefresh() {
        try {
            Mods.save(this, mMods);
        } catch (Exception e) {
            error(e.getMessage());
        }
        refresh();
    }

    private void confirmRemove(String file) {
        new AlertDialog.Builder(this)
                .setTitle(R.string.mods_remove)
                .setMessage(getString(R.string.mods_remove_confirm, file))
                .setPositiveButton(R.string.mods_remove, (d, w) -> {
                    try {
                        Mods.remove(this, file);
                    } catch (Exception e) {
                        error(e.getMessage());
                    }
                    refresh();
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    private void pick() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        startActivityForResult(i, REQ_ADD);
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req != REQ_ADD || res != RESULT_OK || data == null) return;
        List<Uri> uris = new ArrayList<>();
        ClipData clip = data.getClipData();
        if (clip != null)
            for (int i = 0; i < clip.getItemCount(); i++) uris.add(clip.getItemAt(i).getUri());
        else if (data.getData() != null) uris.add(data.getData());
        add(uris);
    }

    private void add(List<Uri> uris) {
        busy(true, 0);
        new Thread(() -> {
            StringBuilder errors = new StringBuilder();
            List<String> added = new ArrayList<>(), updated = new ArrayList<>();
            Uri game = null;
            for (Uri u : uris) {
                try {
                    Mods.Added a = Mods.add(this, u);
                    added.addAll(a.added);
                    updated.addAll(a.updated);
                } catch (Mods.GameArchiveException e) {
                    game = u;
                } catch (Exception e) {
                    errors.append(e.getMessage()).append('\n');
                }
            }
            final Uri fgame = game;
            runOnUiThread(() -> {
                busy(false, 0);
                if (fgame != null) {
                    importGame(fgame);
                    return;
                }
                StringBuilder msg = new StringBuilder();
                if (added.size() == 1) msg.append(getString(R.string.mods_added_one, added.get(0)));
                else if (added.size() > 1) msg.append(getString(R.string.mods_added_many, added.size()));
                if (!updated.isEmpty()) {
                    if (msg.length() > 0) msg.append(' ');
                    msg.append(getString(R.string.mods_updated, String.join(", ", updated)));
                }
                if (errors.length() > 0) {
                    if (msg.length() > 0) msg.append("\n\n");
                    error(msg.append(errors.toString().trim()).toString());
                } else if (msg.length() > 0) {
                    android.widget.Toast.makeText(this, msg, android.widget.Toast.LENGTH_LONG).show();
                }
            });
        }, "mods-add").start();
    }

    private void apply() {
        Mods.Mod[] snapshot = mMods.toArray(new Mods.Mod[0]);
        busy(true, 0);
        new Thread(() -> {
            String err = null;
            int[] shown = {-1};
            try {
                Mods.apply(this, java.util.Arrays.asList(snapshot), (what, done, total) -> {
                    int permille = total > 0 ? (int) ((long) done * 1000 / total) : 0;
                    if (permille / 10 == shown[0]) return;
                    shown[0] = permille / 10;
                    runOnUiThread(() -> busy(true, Math.max(permille, 1)));
                });
            } catch (Exception e) {
                err = e.getMessage() != null ? e.getMessage() : e.toString();
            }
            final String ferr = err;
            runOnUiThread(() -> {
                busy(false, 0);
                if (ferr != null) {
                    error(getString(R.string.mods_apply_failed, ferr));
                    return;
                }
                restart();
            });
        }, "mods-apply").start();
    }

    // A run started with other mods usually can't be loaded after they change
    // (the game can freeze trying), so offer to set it aside first.
    private void checkRun(Runnable then) {
        if (!SaveTransfer.hasRun(this)) {
            then.run();
            return;
        }
        new AlertDialog.Builder(this)
                .setTitle(R.string.mods_run_title)
                .setMessage(R.string.mods_run_body)
                .setPositiveButton(R.string.mods_run_aside, (d, w) -> {
                    try {
                        SaveTransfer.setRunAside(this);
                    } catch (Exception e) {
                        error(e.getMessage());
                        return;
                    }
                    then.run();
                })
                .setNeutralButton(R.string.mods_run_keep, (d, w) -> then.run())
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    private void confirmOff() {
        if (SaveTransfer.hasRun(this)) {
            checkRun(this::turnOff);  // that question is confirmation enough
            return;
        }
        new AlertDialog.Builder(this)
                .setTitle(R.string.mods_turn_off)
                .setMessage(R.string.mods_turn_off_confirm)
                .setPositiveButton(R.string.mods_restart, (d, w) -> turnOff())
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    private void turnOff() {
        Mods.turnOff(this);
        restart();
    }

    private void busy(boolean on, int progress) {
        mProgress.setVisibility(on ? View.VISIBLE : View.GONE);
        mProgress.setIndeterminate(on && progress == 0);
        mProgress.setProgress(progress);
        if (mBusy != on) {
            mBusy = on;
            refresh();
        }
    }

    private void error(String msg) {
        new AlertDialog.Builder(this).setMessage(msg).setPositiveButton(android.R.string.ok, null).show();
    }

    // The game reads its files at startup: start it again with the new set.
    private void restart() {
        Intent i = new Intent(this, GameActivity.class);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(i);
        finishAffinity();
        android.os.Process.killProcess(android.os.Process.myPid());
    }

    private static String size(long bytes) {
        if (bytes < 1 << 20) return Math.max(1, bytes >> 10) + " KB";
        return String.format(Locale.ROOT, "%.1f MB", bytes / 1048576.0);
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
