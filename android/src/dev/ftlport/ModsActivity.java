package dev.ftlport;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.Intent;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.text.format.DateUtils;
import android.text.style.ForegroundColorSpan;
import android.text.style.RelativeSizeSpan;
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
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Adding, ordering and switching on mods, then applying them (see Mods).
 * Also where "Open with FTL" (and sharing a file to FTL) lands: a downloaded
 * mod goes straight into the list, ready to apply. With a mods folder, the
 * list follows the files in it whenever this screen comes up.
 */
public class ModsActivity extends Activity {
    private static final int REQ_ADD = 1, REQ_FOLDER = 2;
    private static final int GREY = 0xFF90A4AE, AMBER = 0xFFFFCC80;

    // What changes the list runs here, one thing at a time, so reading the mods
    // folder can't undo a tick made meanwhile.
    private static final ExecutorService JOBS = Executors.newSingleThreadExecutor();

    private interface Work<T> {
        T run() throws Exception;
    }

    private interface Done<T> {
        void done(T result, Exception error);
    }

    private LinearLayout mList;
    private TextView mStatus, mFolderName, mFolderText, mDoing;
    private Button mApply, mAdd, mOff, mFolderPick, mFolderStop;
    private ProgressBar mProgress;
    private List<Mods.Mod> mMods;
    private List<String> mApplied;
    private int mJobs;          // jobs waiting or running that keep the controls off
    private boolean mCopying;   // a job that doesn't is copying files
    private boolean mShownBusy;
    private String mFolderError;
    private String mShownWhat;  // the progress last shown, kept on the jobs' thread
    private int mShownPercent;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Logs.installCrashHandler(this);
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(20);
        col.setPadding(pad, dp(12), pad, dp(8));

        TextView title = new TextView(this);
        title.setText(R.string.mods_title);
        title.setTextSize(22);
        title.setTextColor(0xFFECEFF1);
        title.setPadding(0, dp(8), 0, dp(4));
        col.addView(title);
        TextView about = new TextView(this);
        about.setText(getString(R.string.mods_about) + "\n\n" + (Importer.isOtherBuild(this)
                ? getString(R.string.mods_about_other_build, Importer.describeCopy(this))
                : getString(SettingsActivity.hyperspaceLib(this) != null ? R.string.mods_about_hs : R.string.mods_about_no_hs)));
        about.setTextSize(14);
        about.setTextColor(GREY);
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

        TextView folder = new TextView(this);
        folder.setText(R.string.mods_folder);
        folder.setTextSize(14);
        folder.setTypeface(Typeface.DEFAULT_BOLD);
        folder.setTextColor(0xFF80CBC4);
        folder.setPadding(0, dp(20), 0, dp(4));
        col.addView(folder);
        mFolderName = new TextView(this);
        mFolderName.setTextSize(15);
        mFolderName.setTextColor(0xFFECEFF1);
        col.addView(mFolderName);
        mFolderText = new TextView(this);
        mFolderText.setTextSize(14);
        mFolderText.setLineSpacing(0, 1.15f);
        col.addView(mFolderText);
        LinearLayout folderButtons = new LinearLayout(this);
        folderButtons.setOrientation(LinearLayout.HORIZONTAL);
        folderButtons.setPadding(0, dp(8), 0, 0);
        mFolderPick = button(folderButtons, R.string.mods_folder_choose, v -> pickFolder());
        mFolderStop = button(folderButtons, R.string.mods_folder_stop, v -> confirmStopFolder());
        col.addView(folderButtons);

        ScrollView sv = new ScrollView(this);
        sv.addView(col);

        // The buttons stay in view below the list, however long it gets.
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.VERTICAL);
        bar.setPadding(pad, 0, pad, dp(12));
        mDoing = new TextView(this);
        mDoing.setTextSize(14);
        mDoing.setTextColor(GREY);
        mDoing.setPadding(0, dp(8), 0, 0);
        bar.addView(mDoing);
        mProgress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        mProgress.setMax(1000);
        bar.addView(mProgress);
        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setPadding(0, dp(8), 0, 0);
        mAdd = button(buttons, R.string.mods_add, v -> pick());
        mApply = button(buttons, R.string.mods_apply, v -> checkRun(this::apply));
        mOff = button(buttons, R.string.mods_turn_off, v -> confirmOff());
        for (Button b : new Button[] {mAdd, mApply, mOff}) {  // they share the width of a phone held upright
            LinearLayout.LayoutParams lp = (LinearLayout.LayoutParams) b.getLayoutParams();
            lp.width = 0;
            lp.weight = 1;
        }
        bar.addView(buttons);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setFitsSystemWindows(true);
        root.addView(sv, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
        root.addView(bar);
        setContentView(root);
        refresh();
        if (state == null) handleIncoming(getIntent());
    }

    @Override
    protected void onResume() {
        super.onResume();
        // The files in the mods folder may have changed meanwhile.
        if (Mods.folder(this) != null)
            job(false, () -> Mods.syncFolder(this, this::progress), (err, e) -> mFolderError = e != null ? message(e) : err);
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

    private boolean busy() {
        return mJobs > 0 || mCopying;
    }

    private void refresh() {
        if (isDestroyed()) return;
        boolean busy = busy();
        mShownBusy = busy;
        mMods = Mods.load(this);
        mApplied = Mods.applied(this);
        mList.removeAllViews();
        if (mMods.isEmpty()) {
            TextView none = new TextView(this);
            none.setText(R.string.mods_none);
            none.setTextSize(15);
            none.setTextColor(0xFFCFD8DC);
            none.setPadding(0, dp(8), 0, dp(8));
            mList.addView(none);
        }
        boolean changed = false;
        for (int i = 0; i < mMods.size(); i++) {
            mList.addView(row(i));
            Mods.Mod m = mMods.get(i);
            if (m.on && m.changed && mApplied.contains(m.file)) changed = true;
        }
        boolean pending = !mApplied.equals(wanted()) || changed;
        if (mApplied.isEmpty()) mStatus.setText(pending ? R.string.mods_status_pending : R.string.mods_status_off);
        else mStatus.setText(getResources().getQuantityString(
                pending ? R.plurals.mods_status_on_pending : R.plurals.mods_status_on, mApplied.size(), mApplied.size()));
        mApply.setEnabled(!busy && pending);
        mOff.setEnabled(!busy && !mApplied.isEmpty());
        mAdd.setEnabled(!busy);

        Uri folder = Mods.folder(this);
        mFolderName.setVisibility(folder != null ? View.VISIBLE : View.GONE);
        if (folder != null) mFolderName.setText(Mods.folderName(this, folder));
        mFolderText.setText(folder == null ? getString(R.string.mods_folder_none)
                : mFolderError != null ? mFolderError : getString(R.string.mods_folder_about));
        mFolderText.setTextColor(folder != null && mFolderError != null ? AMBER : GREY);
        mFolderPick.setText(folder == null ? R.string.mods_folder_choose : R.string.mods_folder_change);
        mFolderStop.setVisibility(folder != null ? View.VISIBLE : View.GONE);
        mFolderPick.setEnabled(!busy);
        mFolderStop.setEnabled(!busy);

        mProgress.setVisibility(busy ? View.VISIBLE : View.GONE);
        mDoing.setVisibility(busy && mDoing.length() > 0 ? View.VISIBLE : View.GONE);
        if (!busy) {
            mDoing.setText("");
            mProgress.setIndeterminate(true);
        }
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
        SpannableStringBuilder text = new SpannableStringBuilder(m.file);
        if (m.problem != null) {
            note(text, m.problem, AMBER);
        } else {
            note(text, details(m), GREY);
            if (m.on && m.changed && mApplied.contains(m.file)) note(text, getString(R.string.mods_changed), AMBER);
        }
        cb.setText(text);
        cb.setTextSize(16);
        cb.setTextColor(0xFFECEFF1);
        cb.setPadding(cb.getPaddingLeft(), dp(6), cb.getPaddingRight(), dp(6));
        cb.setEnabled(!busy() && m.problem == null);
        cb.setOnCheckedChangeListener((b, on) -> job(false, () -> {
            Mods.setOn(this, m.file, on);
            return null;
        }, null));
        row.addView(cb, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        if (m.problem == null && (mMods.size() > 1 || m.from == null)) {
            ImageButton more = new ImageButton(this);
            more.setImageResource(android.R.drawable.ic_menu_more);
            more.setBackgroundColor(0);
            more.setContentDescription(getString(R.string.mods_more));
            more.setEnabled(!busy());
            more.setOnClickListener(v -> menu(v, i));
            row.addView(more);
        }
        return row;
    }

    // A smaller line under a mod's name.
    private static void note(SpannableStringBuilder sb, String line, int color) {
        sb.append('\n');
        int start = sb.length();
        sb.append(line);
        sb.setSpan(new RelativeSizeSpan(0.85f), start, sb.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        sb.setSpan(new ForegroundColorSpan(color), start, sb.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
    }

    // Its size, whether it's from the mods folder, and when this copy was made.
    private String details(Mods.Mod m) {
        String size = size(Mods.size(this, m.file));
        String when = DateUtils.formatDateTime(this, m.updated,
                DateUtils.FORMAT_SHOW_DATE | DateUtils.FORMAT_SHOW_TIME | DateUtils.FORMAT_ABBREV_MONTH);
        boolean updated = m.updated != m.added;
        if (m.from != null)
            return getString(updated ? R.string.mods_row_folder_updated : R.string.mods_row_folder_added, size, when);
        return getString(updated ? R.string.mods_row_updated : R.string.mods_row_added, size, when);
    }

    private void menu(View anchor, int i) {
        Mods.Mod m = mMods.get(i);
        PopupMenu pm = new PopupMenu(this, anchor);
        if (i > 0) pm.getMenu().add(0, 1, 0, R.string.mods_up);
        if (i < mMods.size() - 1) pm.getMenu().add(0, 2, 0, R.string.mods_down);
        if (m.from == null) pm.getMenu().add(0, 3, 0, R.string.mods_remove);  // the folder's go when their files do
        pm.setOnMenuItemClickListener(item -> {
            switch (item.getItemId()) {
                case 1:
                case 2:
                    job(false, () -> {
                        Mods.move(this, m.file, item.getItemId() == 1 ? -1 : 1);
                        return null;
                    }, null);
                    break;
                default:
                    confirmRemove(m.file);
            }
            return true;
        });
        pm.show();
    }

    private void confirmRemove(String file) {
        new AlertDialog.Builder(this)
                .setTitle(R.string.mods_remove)
                .setMessage(getString(R.string.mods_remove_confirm, file))
                .setPositiveButton(R.string.mods_remove, (d, w) -> job(false, () -> {
                    Mods.remove(this, file);
                    return null;
                }, null))
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

    private void pickFolder() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        Uri folder = Mods.folder(this);
        if (folder != null) i.putExtra(DocumentsContract.EXTRA_INITIAL_URI, folder);
        startActivityForResult(i, REQ_FOLDER);
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (res != RESULT_OK || data == null) return;
        if (req == REQ_FOLDER) {
            useFolder(data.getData());
            return;
        }
        if (req != REQ_ADD) return;
        List<Uri> uris = new ArrayList<>();
        ClipData clip = data.getClipData();
        if (clip != null)
            for (int i = 0; i < clip.getItemCount(); i++) uris.add(clip.getItemAt(i).getUri());
        else if (data.getData() != null) uris.add(data.getData());
        add(uris);
    }

    private void useFolder(Uri tree) {
        if (tree == null) return;
        try {
            // Android lends the folder until the app gives it back (when another is chosen).
            getContentResolver().takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException e) {
            error(message(e));
            return;
        }
        mFolderError = null;
        job(true, () -> {
            Mods.setFolder(this, tree);
            return Mods.syncFolder(this, this::progress);
        }, (err, e) -> {
            mFolderError = err;
            if (e != null) error(message(e));
        });
    }

    private void confirmStopFolder() {
        Uri folder = Mods.folder(this);
        if (folder == null) return;
        new AlertDialog.Builder(this)
                .setMessage(getString(R.string.mods_folder_stop_confirm, Mods.folderName(this, folder)))
                .setPositiveButton(R.string.mods_folder_stop, (d, w) -> job(false, () -> {
                    Mods.setFolder(this, null);
                    return null;
                }, (r, e) -> {
                    mFolderError = null;
                    if (e != null) error(message(e));
                }))
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    // What the files added: their mods, the ones they updated, and what went wrong.
    private static final class Outcome {
        final List<String> added = new ArrayList<>(), updated = new ArrayList<>();
        final StringBuilder errors = new StringBuilder();
        Uri game;
    }

    private void add(List<Uri> uris) {
        job(true, () -> {
            Outcome o = new Outcome();
            for (Uri u : uris) {
                try {
                    Mods.Added a = Mods.add(this, u, this::progress);
                    o.added.addAll(a.added);
                    o.updated.addAll(a.updated);
                } catch (Mods.GameArchiveException e) {
                    o.game = u;
                } catch (Exception e) {
                    o.errors.append(e.getMessage()).append('\n');
                }
            }
            return o;
        }, (o, e) -> {
            if (e != null) {
                error(message(e));
                return;
            }
            if (o.game != null) {
                importGame(o.game);
                return;
            }
            StringBuilder msg = new StringBuilder();
            if (o.added.size() == 1) msg.append(getString(R.string.mods_added_one, o.added.get(0)));
            else if (o.added.size() > 1) msg.append(getString(R.string.mods_added_many, o.added.size()));
            if (!o.updated.isEmpty()) {
                if (msg.length() > 0) msg.append(' ');
                msg.append(getString(R.string.mods_updated, String.join(", ", o.updated)));
            }
            if (o.errors.length() > 0) {
                if (msg.length() > 0) msg.append("\n\n");
                error(msg.append(o.errors.toString().trim()).toString());
            } else if (msg.length() > 0) {
                android.widget.Toast.makeText(this, msg, android.widget.Toast.LENGTH_LONG).show();
            }
        });
    }

    private void apply() {
        job(true, () -> {
            Mods.syncFolder(this, this::progress);  // with the latest from the folder
            Mods.apply(this, Mods.load(this), this::progress);
            return null;
        }, (r, e) -> {
            if (e != null) error(getString(R.string.mods_apply_failed, message(e)));
            else restart();
        });
    }

    // Runs work after the jobs before it, then shows the list as it is. A busy
    // job turns the controls off meanwhile; others only once they copy files.
    private <T> void job(boolean busy, Work<T> work, Done<T> done) {
        if (busy) {
            mJobs++;
            refresh();
        }
        JOBS.execute(() -> {
            mShownWhat = null;
            T result = null;
            Exception error = null;
            try {
                result = work.run();
            } catch (Exception e) {
                error = e;
            }
            T r = result;
            Exception err = error;
            runOnUiThread(() -> {
                if (busy) mJobs--;
                mCopying = false;
                if (done != null) done.done(r, err);
                else if (err != null) error(message(err));
                refresh();
            });
        });
    }

    // From a job, on its thread: what it's doing, and how far it got.
    private void progress(String what, int done, int total) {
        int percent = total > 0 ? (int) ((long) done * 100 / total) : 0;
        if (what.equals(mShownWhat) && percent == mShownPercent) return;
        mShownWhat = what;
        mShownPercent = percent;
        runOnUiThread(() -> {
            mCopying = true;
            mDoing.setText(what);
            mProgress.setIndeterminate(total <= 0);
            mProgress.setProgress(Math.max(percent * 10, 10));
            if (!mShownBusy) refresh();
            else mDoing.setVisibility(View.VISIBLE);
        });
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

    private void error(String msg) {
        if (isDestroyed()) return;
        new AlertDialog.Builder(this).setMessage(msg).setPositiveButton(android.R.string.ok, null).show();
    }

    private static String message(Exception e) {
        return e.getMessage() != null ? e.getMessage() : e.toString();
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
