package dev.ftlport;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.net.Uri;
import android.os.Build;
import android.util.DisplayMetrics;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Arrays;
import java.util.Date;
import java.util.Locale;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/**
 * Logs players can send without adb. The native side writes each session's
 * log to files/logs (plat_android.c) and leaves a "crashed" marker when the
 * game crashes; Java crashes are written there too. Settings saves the lot
 * as one zip.
 */
final class Logs {
    private static final int KEEP_JAVA_CRASHES = 5;
    private static boolean sInstalled;

    static File dir(Context ctx) {
        return new File(ctx.getFilesDir(), "logs");
    }

    private static File marker(Context ctx) {
        return new File(dir(ctx), "crashed");
    }

    private static String stamp(String fmt) {
        return new SimpleDateFormat(fmt, Locale.US).format(new Date());
    }

    static void installCrashHandler(Context ctx) {
        if (sInstalled) return;
        sInstalled = true;
        final File dir = dir(ctx.getApplicationContext());
        final Thread.UncaughtExceptionHandler prev = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((t, e) -> {
            try {
                //noinspection ResultOfMethodCallIgnored
                dir.mkdirs();
                File[] old = dir.listFiles((d, n) -> n.startsWith("java-crash-"));
                if (old != null && old.length >= KEEP_JAVA_CRASHES) {
                    Arrays.sort(old);
                    for (int i = 0; i <= old.length - KEEP_JAVA_CRASHES; i++)
                        //noinspection ResultOfMethodCallIgnored
                        old[i].delete();
                }
                File f = new File(dir, "java-crash-" + stamp("yyyyMMdd-HHmmss") + ".txt");
                try (PrintWriter w = new PrintWriter(new FileWriter(f))) {
                    w.println("Uncaught exception in thread \"" + t.getName() + "\"");
                    e.printStackTrace(w);
                }
                //noinspection ResultOfMethodCallIgnored
                new File(dir, "crashed").createNewFile();
            } catch (Throwable ignored) {
            }
            if (prev != null) prev.uncaughtException(t, e);
        });
    }

    static boolean crashedLastTime(Context ctx) {
        return marker(ctx).exists();
    }

    static void clearCrashFlag(Context ctx) {
        //noinspection ResultOfMethodCallIgnored
        marker(ctx).delete();
    }

    static String exportName() {
        return "FTL logs " + stamp("yyyy-MM-dd HHmm") + ".zip";
    }

    static void exportTo(Context ctx, Uri uri) throws IOException {
        File[] files = dir(ctx).listFiles((d, n) -> !n.equals("crashed"));
        if (files == null) files = new File[0];
        Arrays.sort(files);
        try (OutputStream os = ctx.getContentResolver().openOutputStream(uri, "wt")) {
            if (os == null) throw new IOException("Couldn't write the file.");
            ZipOutputStream zip = new ZipOutputStream(os);
            zip.putNextEntry(new ZipEntry("device.txt"));
            zip.write(deviceInfo(ctx).getBytes(StandardCharsets.UTF_8));
            zip.closeEntry();
            byte[] buf = new byte[1 << 14];
            for (File f : files) {
                if (!f.isFile()) continue;
                zip.putNextEntry(new ZipEntry(f.getName()));
                try (InputStream in = new FileInputStream(f)) {
                    int n;
                    while ((n = in.read(buf)) > 0) zip.write(buf, 0, n);
                }
                zip.closeEntry();
            }
            zip.finish();
        }
    }

    private static String deviceInfo(Context ctx) {
        StringBuilder sb = new StringBuilder();
        String version = "?";
        try {
            PackageInfo pi = ctx.getPackageManager().getPackageInfo(ctx.getPackageName(), 0);
            version = pi.versionName;
        } catch (Exception ignored) {
        }
        DisplayMetrics dm = ctx.getResources().getDisplayMetrics();
        sb.append("app ").append(version).append('\n');
        sb.append("device ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
                .append(" (").append(Build.DEVICE).append(")\n");
        sb.append("android ").append(Build.VERSION.RELEASE).append(" (API ").append(Build.VERSION.SDK_INT).append(")\n");
        sb.append("abis ").append(String.join(",", Build.SUPPORTED_ABIS)).append('\n');
        sb.append("display ").append(dm.widthPixels).append('x').append(dm.heightPixels)
                .append(" @ ").append(dm.densityDpi).append(" dpi\n");
        sb.append("game files ").append(Importer.isInstalled(ctx) ? "imported" : "missing").append('\n');
        if (Importer.isInstalled(ctx)) {
            String v = Importer.version(ctx);
            sb.append("game version ").append(v != null ? v : "unknown").append(", build ").append(Importer.buildId(ctx))
                    .append(Importer.isOtherBuild(ctx) ? " (not the 1.6.21 build)" : " (1.6.21)").append('\n');
        }
        SharedPreferences prefs = ctx.getSharedPreferences(SettingsActivity.PREFS, Context.MODE_PRIVATE);
        for (Map.Entry<String, ?> e : prefs.getAll().entrySet())
            sb.append("setting ").append(e.getKey()).append(" = ").append(e.getValue()).append('\n');
        File[] saves = SaveTransfer.docsDir(ctx).listFiles();
        if (saves != null)
            for (File f : saves) sb.append("save ").append(f.getName()).append(' ').append(f.length()).append('\n');
        return sb.toString();
    }
}
