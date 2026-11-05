package dev.ftlport;

import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.PackageInstaller;
import android.os.Build;
import android.util.Log;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Locale;

/**
 * The built-in updater of release builds (BuildInfo.UPDATE_REPO, set by
 * RELEASE=1 android/build-apk.sh): at start it asks GitHub for the repository's
 * latest release. When that is newer than this app and has an .apk, it offers
 * it: Update downloads it, checks its size and SHA-256 against the release and
 * hands it to Android's package installer, which asks the player to confirm.
 * Not now asks again next time, Skip this version doesn't. The setting
 * "Look for updates" turns it off. Builds without RELEASE=1 have no updater and
 * don't ask for the network.
 */
final class Updater {
    private static final String TAG = "FTLPort";
    static final String K_CHECK = "check_updates";  // boolean
    private static final String K_SKIP = "update_skip";  // String: the release the player skipped
    private static final String ACTION_INSTALL = "dev.ftlport.UPDATE_INSTALLED";
    private static final int TIMEOUT_MS = 10000;
    private static final int MAX_JSON = 1 << 20;
    private static final long MAX_APK = 512L << 20;
    private static boolean sChecked;  // once per process

    private Updater() {}

    static boolean available() {
        return !BuildInfo.UPDATE_REPO.isEmpty();
    }

    private static final class Release {
        String tag, url, sha256;
        long size = -1;
    }

    /** At start: looks for a newer release in the background and offers it. */
    static void check(GameActivity a) {
        if (!available() || sChecked) return;
        sChecked = true;
        SharedPreferences prefs = a.getSharedPreferences(SettingsActivity.PREFS, Context.MODE_PRIVATE);
        deleteTree(dir(a));  // what an earlier update downloaded
        if (!prefs.getBoolean(K_CHECK, true)) return;
        String mine = version(a);
        new Thread(() -> {
            Release r;
            try {
                r = latest();
            } catch (Exception e) {
                Log.w(TAG, "update: no answer from GitHub (" + e + ")");
                return;
            }
            if (r == null) {
                Log.i(TAG, "update: " + BuildInfo.UPDATE_REPO + " has no releases");
            } else if (!newer(r.tag, mine)) {
                Log.i(TAG, "update: " + mine + " is the latest release");
            } else if (r.url == null) {
                Log.w(TAG, "update: release " + r.tag + " has no .apk");
            } else if (r.tag.equals(prefs.getString(K_SKIP, null))) {
                Log.i(TAG, "update: " + r.tag + " available, skipped by the player");
            } else {
                Log.i(TAG, "update: " + r.tag + " available (this is " + mine + ")");
                a.runOnUiThread(() -> offer(a, r, mine));
            }
        }, "updater").start();
    }

    private static void offer(GameActivity a, Release r, String mine) {
        if (a.isFinishing()) return;
        SharedPreferences prefs = a.getSharedPreferences(SettingsActivity.PREFS, Context.MODE_PRIVATE);
        a.showDialog(new AlertDialog.Builder(a)
                .setTitle(R.string.update_title)
                .setMessage(a.getString(R.string.update_body, r.tag, mine))
                .setPositiveButton(R.string.update_now, (d, w) -> download(a, r))
                .setNegativeButton(R.string.update_later, null)
                .setNeutralButton(R.string.update_skip, (d, w) -> prefs.edit().putString(K_SKIP, r.tag).apply())
                .setOnDismissListener(d -> a.dialogClosed())
                .create());
    }

    // ---- GitHub ----

    private static HttpURLConnection open(String url) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        if (!"https".equals(c.getURL().getProtocol())) throw new IOException("not https: " + url);
        c.setConnectTimeout(TIMEOUT_MS);
        c.setReadTimeout(TIMEOUT_MS);
        c.setInstanceFollowRedirects(true);  // https to https only, as HttpURLConnection does
        c.setRequestProperty("User-Agent", "ftl-android-updater");
        return c;
    }

    /** The latest release, or null when the repository has none (or is gone). */
    private static Release latest() throws Exception {
        HttpURLConnection c = open("https://api.github.com/repos/" + BuildInfo.UPDATE_REPO + "/releases/latest");
        c.setRequestProperty("Accept", "application/vnd.github+json");
        try {
            int status = c.getResponseCode();
            if (status == 404) return null;
            if (status != 200) throw new IOException("HTTP " + status);
            ByteArrayOutputStream b = new ByteArrayOutputStream();
            try (InputStream in = c.getInputStream()) {
                byte[] buf = new byte[16384];
                for (int n; (n = in.read(buf)) > 0; ) {
                    b.write(buf, 0, n);
                    if (b.size() > MAX_JSON) throw new IOException("answer too large");
                }
            }
            JSONObject j = new JSONObject(new String(b.toByteArray(), StandardCharsets.UTF_8));
            Release r = new Release();
            r.tag = j.getString("tag_name");
            JSONArray assets = j.optJSONArray("assets");
            for (int i = 0; assets != null && i < assets.length(); i++) {
                JSONObject asset = assets.getJSONObject(i);
                if (!asset.optString("name").toLowerCase(Locale.ROOT).endsWith(".apk")) continue;
                r.url = asset.getString("browser_download_url");
                r.size = asset.optLong("size", -1);
                String digest = asset.optString("digest", "");
                if (digest.startsWith("sha256:") && digest.length() == 7 + 64) r.sha256 = digest.substring(7);
                break;
            }
            return r;
        } finally {
            c.disconnect();
        }
    }

    // ---- versions: X.Y.Z, with or without a leading v ----

    private static int[] parse(String v) {
        if (v == null) return null;
        String[] p = (v.startsWith("v") ? v.substring(1) : v).split("\\.");
        if (p.length != 3) return null;
        try {
            return new int[] {Integer.parseInt(p[0]), Integer.parseInt(p[1]), Integer.parseInt(p[2])};
        } catch (NumberFormatException e) {
            return null;
        }
    }

    static boolean newer(String tag, String mine) {
        int[] a = parse(tag), b = parse(mine);
        if (a == null || b == null) return false;
        for (int i = 0; i < 3; i++)
            if (a[i] != b[i]) return a[i] > b[i];
        return false;
    }

    private static String version(Context ctx) {
        try {
            return ctx.getPackageManager().getPackageInfo(ctx.getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "";
        }
    }

    // ---- download and install ----

    private static File dir(Context ctx) {
        return new File(ctx.getCacheDir(), "update");
    }

    private static void download(GameActivity a, Release r) {
        LinearLayout col = new LinearLayout(a);
        col.setOrientation(LinearLayout.VERTICAL);
        int pad = Math.round(24 * a.getResources().getDisplayMetrics().density);
        col.setPadding(pad, pad / 2, pad, 0);
        ProgressBar bar = new ProgressBar(a, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        TextView status = new TextView(a);
        col.addView(bar);
        col.addView(status);
        boolean[] cancelled = {false};
        AlertDialog progress = new AlertDialog.Builder(a)
                .setTitle(a.getString(R.string.update_downloading, r.tag))
                .setView(col)
                .setCancelable(false)
                .setNegativeButton(android.R.string.cancel, (d, w) -> cancelled[0] = true)
                .setOnDismissListener(d -> a.dialogClosed())
                .create();
        a.showDialog(progress);
        File apk = new File(dir(a), "ftl.apk");
        new Thread(() -> {
            String error = null;
            try {
                fetch(r, apk, cancelled, (got, total) -> a.runOnUiThread(() -> {
                    if (total > 0) bar.setProgress((int) (got * 1000 / total));
                    status.setText(String.format(Locale.ROOT, "%d / %d MB", got >> 20, Math.max(total, 0) >> 20));
                }));
                if (!cancelled[0]) install(a, apk);
            } catch (Exception e) {
                Log.e(TAG, "update: download failed", e);
                error = e.getMessage() != null ? e.getMessage() : e.toString();
            }
            String err = error;
            a.runOnUiThread(() -> {
                progress.dismiss();
                if (err != null && !cancelled[0]) failed(a, err);
            });
            if (err != null || cancelled[0]) deleteTree(dir(a));
        }, "updater").start();
    }

    private interface Progress {
        void at(long got, long total);
    }

    private static void fetch(Release r, File apk, boolean[] cancelled, Progress progress) throws Exception {
        deleteTree(apk.getParentFile());
        //noinspection ResultOfMethodCallIgnored
        apk.getParentFile().mkdirs();
        HttpURLConnection c = open(r.url);
        try {
            int status = c.getResponseCode();
            if (status != 200) throw new IOException("HTTP " + status);
            MessageDigest sha = MessageDigest.getInstance("SHA-256");
            long got = 0, total = r.size > 0 ? r.size : c.getContentLengthLong(), shown = 0;
            try (InputStream in = c.getInputStream(); OutputStream out = new FileOutputStream(apk)) {
                byte[] buf = new byte[65536];
                for (int n; (n = in.read(buf)) > 0; ) {
                    if (cancelled[0]) return;
                    out.write(buf, 0, n);
                    sha.update(buf, 0, n);
                    got += n;
                    if (got > MAX_APK) throw new IOException("the download is too large");
                    if (got - shown >= 256 * 1024) {
                        progress.at(got, total);
                        shown = got;
                    }
                }
            }
            progress.at(got, total);
            if (r.size > 0 && got != r.size)
                throw new IOException("download incomplete (" + got + " of " + r.size + " bytes)");
            if (r.sha256 != null) {
                StringBuilder hex = new StringBuilder();
                for (byte b : sha.digest()) hex.append(String.format(Locale.ROOT, "%02x", b));
                if (!hex.toString().equalsIgnoreCase(r.sha256))
                    throw new IOException("the download doesn't match the release's checksum");
            }
        } finally {
            c.disconnect();
        }
    }

    // Android's installer checks that the APK is signed like this app and asks the
    // player to confirm; the app closes while it installs.
    private static void install(GameActivity a, File apk) throws IOException {
        Context app = a.getApplicationContext();
        PackageInstaller installer = app.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params =
                new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(app.getPackageName());
        params.setSize(apk.length());
        int id = installer.createSession(params);
        try (PackageInstaller.Session s = installer.openSession(id)) {
            try (InputStream in = new FileInputStream(apk); OutputStream out = s.openWrite("ftl.apk", 0, apk.length())) {
                byte[] buf = new byte[65536];
                for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
                s.fsync(out);
            }
            IntentFilter filter = new IntentFilter(ACTION_INSTALL);
            BroadcastReceiver receiver = new BroadcastReceiver() {
                @Override
                public void onReceive(Context ctx, Intent i) {
                    onInstallStatus(a, i);
                }
            };
            if (Build.VERSION.SDK_INT >= 33) app.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED);
            else app.registerReceiver(receiver, filter);
            Intent done = new Intent(ACTION_INSTALL).setPackage(app.getPackageName());
            int flags = PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0);
            s.commit(PendingIntent.getBroadcast(app, id, done, flags).getIntentSender());
            Log.i(TAG, "update: handed " + apk.length() + " bytes to the installer");
        }
    }

    @SuppressWarnings("deprecation")
    private static void onInstallStatus(GameActivity a, Intent i) {
        int status = i.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
        if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            Intent confirm = i.getParcelableExtra(Intent.EXTRA_INTENT);
            if (confirm != null) {
                confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                a.getApplicationContext().startActivity(confirm);
            }
        } else if (status != PackageInstaller.STATUS_SUCCESS) {
            String msg = i.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
            Log.w(TAG, "update: not installed (" + status + ", " + msg + ")");
            // Cancelled by the player: nothing to say
            if (status != PackageInstaller.STATUS_FAILURE_ABORTED && !a.isFinishing())
                failed(a, msg != null ? msg : String.valueOf(status));
        }
    }

    private static void failed(GameActivity a, String why) {
        if (a.isFinishing()) return;
        a.showDialog(new AlertDialog.Builder(a)
                .setTitle(R.string.update_failed_title)
                .setMessage(a.getString(R.string.update_failed_body, why))
                .setPositiveButton(android.R.string.ok, null)
                .setOnDismissListener(d -> a.dialogClosed())
                .create());
    }

    private static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }
}
