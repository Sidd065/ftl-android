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
import java.util.List;
import java.util.Locale;

/** Adding, ordering and switching on mods, then applying them (see Mods). */
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
        about.setText(R.string.mods_about);
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
        mApply = button(buttons, R.string.mods_apply, v -> apply());
        mOff = button(buttons, R.string.mods_turn_off, v -> confirmOff());
        col.addView(buttons);

        ScrollView sv = new ScrollView(this);
        sv.setFitsSystemWindows(true);
        sv.addView(col);
        setContentView(sv);
        refresh();
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
        busy(true, 0);
        new Thread(() -> {
            StringBuilder errors = new StringBuilder();
            for (Uri u : uris) {
                try {
                    Mods.add(this, u);
                } catch (Exception e) {
                    errors.append(e.getMessage()).append('\n');
                }
            }
            runOnUiThread(() -> {
                busy(false, 0);
                if (errors.length() > 0) error(errors.toString().trim());
            });
        }, "mods-add").start();
    }

    private void apply() {
        Mods.Mod[] snapshot = mMods.toArray(new Mods.Mod[0]);
        busy(true, 0);
        new Thread(() -> {
            String err = null;
            Mods.Result r = null;
            int[] shown = {-1};
            try {
                r = Mods.apply(this, java.util.Arrays.asList(snapshot), (what, done, total) -> {
                    int permille = total > 0 ? (int) ((long) done * 1000 / total) : 0;
                    if (permille / 10 == shown[0]) return;
                    shown[0] = permille / 10;
                    runOnUiThread(() -> busy(true, Math.max(permille, 1)));
                });
            } catch (Exception e) {
                err = e.getMessage() != null ? e.getMessage() : e.toString();
            }
            final String ferr = err;
            final Mods.Result fr = r;
            runOnUiThread(() -> {
                busy(false, 0);
                if (ferr != null) {
                    error(getString(R.string.mods_apply_failed, ferr));
                    return;
                }
                if (!fr.notes.isEmpty()) {
                    new AlertDialog.Builder(this)
                            .setTitle(R.string.mods_notes_title)
                            .setMessage(join(fr.notes))
                            .setPositiveButton(R.string.mods_restart, (d, w) -> restart())
                            .setCancelable(false)
                            .show();
                } else {
                    restart();
                }
            });
        }, "mods-apply").start();
    }

    private void confirmOff() {
        new AlertDialog.Builder(this)
                .setTitle(R.string.mods_turn_off)
                .setMessage(R.string.mods_turn_off_confirm)
                .setPositiveButton(R.string.mods_restart, (d, w) -> {
                    Mods.turnOff(this);
                    restart();
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    private static String join(List<String> notes) {
        StringBuilder sb = new StringBuilder();
        int n = Math.min(notes.size(), 12);
        for (int i = 0; i < n; i++) sb.append("• ").append(notes.get(i)).append('\n');
        if (notes.size() > n) sb.append("… ").append(notes.size() - n).append(" more");
        return sb.toString().trim();
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
