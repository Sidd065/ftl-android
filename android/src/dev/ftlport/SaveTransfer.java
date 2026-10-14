package dev.ftlport;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

/**
 * Moves saves in and out of the app. The game keeps them in files/Documents:
 * continue.sav (the run in progress), prof.sav (unlocks, achievements and
 * scores) and settings.ini. Export zips them; import takes such a zip or the
 * files on their own, and backs up whatever it replaces.
 */
final class SaveTransfer {
    static final String RUN = "continue.sav", PROFILE = "prof.sav", SETTINGS = "settings.ini",
            DEFAULTS = "userdefaults.txt";
    private static final List<String> FILES = Arrays.asList(RUN, PROFILE, SETTINGS, DEFAULTS);
    // Each save starts with its format number; these are the ones FTL 1.6 writes
    // (the desktop game's ae_prof.sav uses the profile format too).
    private static final int RUN_FORMAT = 11, PROFILE_FORMAT = 9;
    private static final int MAX_FILE = 16 << 20;
    private static final int KEEP_BACKUPS = 3;

    static File docsDir(Context ctx) {
        return new File(ctx.getFilesDir(), "Documents");
    }

    private static File backupDir(Context ctx) {
        return new File(ctx.getFilesDir(), "save-backups");
    }

    private static String stamp() {
        return new SimpleDateFormat("yyyy-MM-dd HHmm", Locale.US).format(new Date());
    }

    static String exportName() {
        return "FTL saves " + stamp() + ".zip";
    }

    private static List<File> present(Context ctx) {
        List<File> out = new ArrayList<>();
        for (String n : FILES) {
            File f = new File(docsDir(ctx), n);
            if (f.isFile() && f.length() > 0) out.add(f);
        }
        return out;
    }

    static boolean hasSaves(Context ctx) {
        for (File f : present(ctx))
            if (!f.getName().equals(DEFAULTS)) return true;
        return false;
    }

    static void exportTo(Context ctx, Uri uri) throws IOException {
        try (OutputStream os = ctx.getContentResolver().openOutputStream(uri, "wt")) {
            if (os == null) throw new IOException("Couldn't write the file.");
            zip(present(ctx), os);
        }
    }

    private static void zip(List<File> files, OutputStream os) throws IOException {
        ZipOutputStream zip = new ZipOutputStream(os);
        byte[] buf = new byte[1 << 14];
        for (File f : files) {
            zip.putNextEntry(new ZipEntry(f.getName()));
            try (InputStream in = new FileInputStream(f)) {
                int n;
                while ((n = in.read(buf)) > 0) zip.write(buf, 0, n);
            }
            zip.closeEntry();
        }
        zip.finish();
    }

    /** Reads the picked files (zips or single saves): name it will be stored under -> contents. */
    static Map<String, byte[]> read(Context ctx, List<Uri> uris) throws IOException {
        ContentResolver cr = ctx.getContentResolver();
        Map<String, byte[]> out = new LinkedHashMap<>();
        for (Uri u : uris) {
            byte[] data;
            try (InputStream in = cr.openInputStream(u)) {
                if (in == null) throw new IOException("Couldn't open the file.");
                data = readAll(in, MAX_FILE * 4);
            }
            if (data.length >= 4 && data[0] == 'P' && data[1] == 'K' && data[2] == 3 && data[3] == 4) {
                readZip(ctx, data, out);
                continue;
            }
            String name = displayName(cr, u);
            String as = storeName(ctx, name, data);
            if (as == null) throw new IOException(ctx.getString(R.string.saves_not_a_save, name));
            out.put(as, data);
        }
        if (out.isEmpty()) throw new IOException(ctx.getString(R.string.saves_none_found));
        return out;
    }

    private static void readZip(Context ctx, byte[] data, Map<String, byte[]> out) throws IOException {
        try (ZipInputStream zip = new ZipInputStream(new java.io.ByteArrayInputStream(data))) {
            ZipEntry e;
            while ((e = zip.getNextEntry()) != null) {
                String path = e.getName();
                if (e.isDirectory() || path.contains("__MACOSX")) continue;
                String name = path.substring(path.lastIndexOf('/') + 1);
                byte[] body = readAll(zip, MAX_FILE);
                String as = storeName(ctx, name, body);
                // a prof.sav wins over a desktop ae_prof.sav in the same zip
                if (as != null && !(as.equals(PROFILE) && out.containsKey(PROFILE) && !name.equals(PROFILE)))
                    out.put(as, body);
            }
        }
    }

    // Which save `data` is, from its name and format number; null if it isn't one.
    private static String storeName(Context ctx, String name, byte[] data) throws IOException {
        String n = name.toLowerCase(Locale.ROOT);
        if (n.equals(DEFAULTS)) return DEFAULTS;
        if (n.endsWith(".ini")) return n.startsWith("settings") ? SETTINGS : null;
        if (!n.endsWith(".sav")) return null;
        int format = data.length >= 4 ? le32(data, 0) : -1;
        if (format == RUN_FORMAT) return RUN;
        if (format == PROFILE_FORMAT) return PROFILE;
        throw new IOException(ctx.getString(R.string.saves_bad_format, name));
    }

    /** One line per file, for the confirmation dialog. */
    static String describe(Context ctx, Map<String, byte[]> files) {
        StringBuilder sb = new StringBuilder();
        for (String n : files.keySet()) {
            String line;
            if (n.equals(RUN)) {
                String[] ship = runSummary(files.get(n));
                line = ship != null ? ctx.getString(R.string.saves_item_run, ship[0], Integer.parseInt(ship[1]))
                        : ctx.getString(R.string.saves_item_run_plain);
            } else if (n.equals(PROFILE)) {
                line = ctx.getString(R.string.saves_item_profile);
            } else if (n.equals(SETTINGS)) {
                line = ctx.getString(R.string.saves_item_settings);
            } else {
                continue;
            }
            if (sb.length() > 0) sb.append('\n');
            sb.append(line);
        }
        return sb.toString();
    }

    // Ship name and sector from the start of a continue.sav: format, three
    // flags, four profile counters, then the ship's name and blueprint.
    private static String[] runSummary(byte[] d) {
        try {
            int p = 32;
            int len = le32(d, p);
            if (len <= 0 || len > 64) return null;
            String name = new String(d, p + 4, len, StandardCharsets.UTF_8);
            p += 4 + len;
            p += 4 + le32(d, p);
            int sector = le32(d, p);
            if (sector < 1 || sector > 8) return null;
            return new String[] {name, Integer.toString(sector)};
        } catch (RuntimeException e) {
            return null;
        }
    }

    /** Replaces saves with `files`, after backing up the current ones. */
    static void install(Context ctx, Map<String, byte[]> files) throws IOException {
        File docs = docsDir(ctx);
        //noinspection ResultOfMethodCallIgnored
        docs.mkdirs();
        backUp(ctx);
        for (Map.Entry<String, byte[]> e : files.entrySet()) write(new File(docs, e.getKey()), e.getValue());
    }

    static boolean hasRun(Context ctx) {
        return new File(docsDir(ctx), RUN).length() > 0;
    }

    /** Moves the run in progress out of the game's way. "Restore earlier saves" brings it back. */
    static void setRunAside(Context ctx) throws IOException {
        if (!hasRun(ctx)) return;
        backUp(ctx);
        if (!new File(docsDir(ctx), RUN).delete()) throw new IOException("Couldn't set the run aside");
    }

    private static void backUp(Context ctx) throws IOException {
        File dir = backupDir(ctx);
        //noinspection ResultOfMethodCallIgnored
        dir.mkdirs();
        File out = new File(dir, "saves " + stamp() + " " + System.currentTimeMillis() % 100000 + ".zip");
        try (OutputStream os = new FileOutputStream(out)) {
            zip(present(ctx), os);
        }
        File[] all = backups(ctx);
        for (int i = 0; i + KEEP_BACKUPS < all.length; i++)
            //noinspection ResultOfMethodCallIgnored
            all[i].delete();
    }

    // oldest first
    private static File[] backups(Context ctx) {
        File[] all = backupDir(ctx).listFiles((d, n) -> n.endsWith(".zip"));
        if (all == null) return new File[0];
        Arrays.sort(all, (a, b) -> Long.compare(a.lastModified(), b.lastModified()));
        return all;
    }

    static boolean canUndo(Context ctx) {
        return backups(ctx).length > 0;
    }

    /** Puts back the saves from before the last import, or before a run was set aside. */
    static void undoImport(Context ctx) throws IOException {
        File[] all = backups(ctx);
        if (all.length == 0) return;
        File last = all[all.length - 1];
        Map<String, byte[]> files = new LinkedHashMap<>();
        try (ZipInputStream zip = new ZipInputStream(new FileInputStream(last))) {
            ZipEntry e;
            while ((e = zip.getNextEntry()) != null)
                if (FILES.contains(e.getName())) files.put(e.getName(), readAll(zip, MAX_FILE));
        }
        File docs = docsDir(ctx);
        for (String n : FILES) {
            File f = new File(docs, n);
            if (files.containsKey(n)) write(f, files.get(n));
            else //noinspection ResultOfMethodCallIgnored
                f.delete();
        }
        //noinspection ResultOfMethodCallIgnored
        last.delete();
    }

    private static void write(File f, byte[] data) throws IOException {
        File tmp = new File(f.getPath() + ".tmp");
        try (FileOutputStream os = new FileOutputStream(tmp)) {
            os.write(data);
            os.getFD().sync();
        }
        if (!tmp.renameTo(f)) throw new IOException("Couldn't write " + f.getName());
    }

    private static byte[] readAll(InputStream in, int limit) throws IOException {
        ByteArrayOutputStream bo = new ByteArrayOutputStream();
        byte[] buf = new byte[1 << 14];
        int n;
        while ((n = in.read(buf)) > 0) {
            bo.write(buf, 0, n);
            if (bo.size() > limit) throw new IOException("That file is too big to be an FTL save.");
        }
        return bo.toByteArray();
    }

    private static String displayName(ContentResolver cr, Uri uri) {
        try (Cursor c = cr.query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getString(0);
        } catch (Exception ignored) {
        }
        String p = uri.getLastPathSegment();
        return p != null ? p.substring(p.lastIndexOf('/') + 1) : "file";
    }

    private static int le32(byte[] b, int o) {
        return (b[o] & 0xff) | (b[o + 1] & 0xff) << 8 | (b[o + 2] & 0xff) << 16 | (b[o + 3] & 0xff) << 24;
    }
}
