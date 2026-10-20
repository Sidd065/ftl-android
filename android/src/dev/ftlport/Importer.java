package dev.ftlport;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Copies the two files the port needs (the iOS executable "FTL" and the data
 * package "ftl.dat") out of the user's own FTL for iPad .ipa (a zip), or out of
 * an extracted FTL.app folder, into app-private storage. Info.plist comes along
 * when it's there, for the game's version.
 */
final class Importer {
    // The build of the game the app's game hooks and Hyperspace are made for: the
    // LC_UUID of FTL for iPad 1.6.21 (arm64). native/src/ux/ux.c checks the same.
    private static final String SUPPORTED_BUILD = "873d2b34d7073d6bab83267bf4c5bbc0";

    interface Progress {
        void onProgress(String what, long done, long total);
    }

    static File gameDir(Context ctx) {
        return new File(ctx.getFilesDir(), "game");
    }

    static boolean isInstalled(Context ctx) {
        File d = gameDir(ctx);
        return new File(d, "FTL").length() > 1_000_000 && new File(d, "ftl.dat").length() > 10_000_000;
    }

    /** Import from a .ipa/.zip document. */
    static void importArchive(Context ctx, Uri uri, Progress p) throws IOException {
        File dir = gameDir(ctx);
        File tmp = new File(ctx.getFilesDir(), "import.tmp");
        deleteRecursive(tmp);
        if (!tmp.mkdirs()) throw new IOException("cannot create " + tmp);
        long total = querySize(ctx.getContentResolver(), uri);
        boolean gotBin = false, gotDat = false;
        try (InputStream raw = ctx.getContentResolver().openInputStream(uri);
             CountingStream counting = new CountingStream(new BufferedInputStream(raw, 1 << 16));
             ZipInputStream zip = new ZipInputStream(counting)) {
            ZipEntry e;
            byte[] buf = new byte[1 << 16];
            while ((e = zip.getNextEntry()) != null) {
                String name = e.getName();
                String target = null;
                if (isAppFile(name, "FTL")) target = "FTL";
                else if (isAppFile(name, "ftl.dat")) target = "ftl.dat";
                else if (isAppFile(name, "Info.plist")) target = "Info.plist";
                if (target == null) continue;
                File out = new File(tmp, target);
                try (OutputStream os = new FileOutputStream(out)) {
                    int n;
                    while ((n = zip.read(buf)) > 0) {
                        os.write(buf, 0, n);
                        p.onProgress(target, counting.count, total);
                    }
                }
                if (target.equals("FTL")) gotBin = true;
                else if (target.equals("ftl.dat")) gotDat = true;
            }
        }
        if (!gotBin || !gotDat)
            throw new IOException("This file doesn't look like FTL for iPad: the files FTL and ftl.dat (from FTL.app) were not found in it.");
        finish(ctx, tmp, dir);
    }

    /** Import from an extracted folder (FTL.app or a parent containing Payload/FTL.app). */
    static void importTree(Context ctx, Uri tree, Progress p) throws IOException {
        ContentResolver cr = ctx.getContentResolver();
        String rootId = DocumentsContract.getTreeDocumentId(tree);
        String appDir = findAppDir(cr, tree, rootId, 0);
        if (appDir == null) throw new IOException("Couldn't find FTL and ftl.dat in that folder.");
        File tmp = new File(ctx.getFilesDir(), "import.tmp");
        deleteRecursive(tmp);
        if (!tmp.mkdirs()) throw new IOException("cannot create " + tmp);
        for (String f : new String[] {"FTL", "ftl.dat", "Info.plist"}) {
            String id = childId(cr, tree, appDir, f);
            if (id == null && f.equals("Info.plist")) continue;
            if (id == null) throw new IOException("Missing " + f);
            Uri u = DocumentsContract.buildDocumentUriUsingTree(tree, id);
            long total = querySize(cr, u);
            try (InputStream in = cr.openInputStream(u); OutputStream os = new FileOutputStream(new File(tmp, f))) {
                byte[] buf = new byte[1 << 16];
                long done = 0;
                int n;
                while ((n = in.read(buf)) > 0) {
                    os.write(buf, 0, n);
                    done += n;
                    p.onProgress(f, done, total);
                }
            }
        }
        finish(ctx, tmp, gameDir(ctx));
    }

    /** Import files someone copied into Android/data/<pkg>/files (FTL + ftl.dat, and Info.plist if there). */
    static boolean importFromExternalDir(Context ctx, Progress p) throws IOException {
        File ext = ctx.getExternalFilesDir(null);
        if (ext == null) return false;
        File bin = new File(ext, "FTL"), dat = new File(ext, "ftl.dat");
        if (!bin.isFile() || !dat.isFile()) return false;
        File tmp = new File(ctx.getFilesDir(), "import.tmp");
        deleteRecursive(tmp);
        if (!tmp.mkdirs()) throw new IOException("cannot create " + tmp);
        File plist = new File(ext, "Info.plist");
        copyFile(bin, new File(tmp, "FTL"), p);
        copyFile(dat, new File(tmp, "ftl.dat"), p);
        if (plist.isFile()) copyFile(plist, new File(tmp, "Info.plist"), p);
        finish(ctx, tmp, gameDir(ctx));
        // The copies in shared storage are no longer needed (saves ~240 MB).
        //noinspection ResultOfMethodCallIgnored
        bin.delete();
        //noinspection ResultOfMethodCallIgnored
        dat.delete();
        //noinspection ResultOfMethodCallIgnored
        plist.delete();
        return true;
    }

    private static void finish(Context ctx, File tmp, File dir) throws IOException {
        String why = checkExecutable(new File(tmp, "FTL"));
        if (why != null) {
            deleteRecursive(tmp);
            throw new IOException(why);
        }
        deleteRecursive(dir);
        if (!tmp.renameTo(dir)) throw new IOException("cannot move imported files into place");
        Mods.turnOff(ctx);  // any mods were applied to the old copy
    }

    /** Returns null if usable, else a human-readable reason. */
    static String checkExecutable(File f) {
        try (RandomAccessFile r = new RandomAccessFile(f, "r")) {
            long slice = arm64Slice(r);
            if (slice < 0) return "The FTL executable has no 64-bit (arm64) code.";
            byte[] cmds = loadCommands(r, slice);
            if (cmds == null) return "The FTL executable isn't a 64-bit iOS binary.";
            int ncmds = le32(cmds, 0), p = 4;
            for (int i = 0; i < ncmds && p + 8 <= cmds.length; i++) {
                int cmd = le32(cmds, p), size = le32(cmds, p + 4);
                if (cmd == 0x2C && le32(cmds, p + 16) != 0)
                    return "This FTL executable is still FairPlay-encrypted, so it can't run here. "
                         + "An unencrypted copy of the app is required.";
                if (size <= 0) break;
                p += size;
            }
            return null;
        } catch (IOException e) {
            return "Couldn't read the FTL executable: " + e.getMessage();
        }
    }

    /** The imported executable's LC_UUID in hex, which tells builds of the game apart; null if unreadable. */
    static String buildId(Context ctx) {
        try (RandomAccessFile r = new RandomAccessFile(new File(gameDir(ctx), "FTL"), "r")) {
            long slice = arm64Slice(r);
            byte[] cmds = slice < 0 ? null : loadCommands(r, slice);
            if (cmds == null) return null;
            int ncmds = le32(cmds, 0), p = 4;
            for (int i = 0; i < ncmds && p + 24 <= cmds.length; i++) {
                int cmd = le32(cmds, p), size = le32(cmds, p + 4);
                if (cmd == 0x1B) {  // LC_UUID
                    StringBuilder sb = new StringBuilder();
                    for (int j = 0; j < 16; j++)
                        sb.append(Character.forDigit((cmds[p + 8 + j] >> 4) & 15, 16)).append(Character.forDigit(cmds[p + 8 + j] & 15, 16));
                    return sb.toString();
                }
                if (size <= 0) break;
                p += size;
            }
        } catch (IOException ignored) {
        }
        return null;
    }

    /**
     * The imported game is another build than FTL for iPad 1.6.21. It runs, but
     * Hyperspace and the extras that hook the game's code (mods' pictures, the
     * two-finger pause, Back, keyboard shortcuts) only know 1.6.21.
     */
    static boolean isOtherBuild(Context ctx) {
        return isInstalled(ctx) && !SUPPORTED_BUILD.equals(buildId(ctx));
    }

    /** The game's version from its Info.plist (imported since 0.4.2), or null. */
    static String version(Context ctx) {
        File f = new File(gameDir(ctx), "Info.plist");
        if (!f.isFile() || f.length() > 1 << 20) return null;
        byte[] b = new byte[(int) f.length()];
        try (RandomAccessFile r = new RandomAccessFile(f, "r")) {
            r.readFully(b);
        } catch (IOException e) {
            return null;
        }
        try {
            if (b.length > 40 && new String(b, 0, 8, StandardCharsets.US_ASCII).equals("bplist00"))
                return binaryPlistString(b, "CFBundleShortVersionString");
        } catch (RuntimeException badPlist) {
            return null;
        }
        Matcher m = Pattern.compile("<key>CFBundleShortVersionString</key>\\s*<string>([^<]{1,40})</string>")
                .matcher(new String(b, StandardCharsets.UTF_8));
        return m.find() ? m.group(1).trim() : null;
    }

    /** The player's copy of the game, for messages: its version when known. */
    static String describeCopy(Context ctx) {
        String v = version(ctx);
        return v != null && !v.equals("1.6.21") ? ctx.getString(R.string.copy_version, v)
                : ctx.getString(R.string.copy_other_build);
    }

    // A top-level string in a binary property list, as Xcode writes Info.plist.
    // The trailer gives the sizes of offsets and object references, the object
    // count, the top object (a dictionary here) and where the offset table is.
    private static String binaryPlistString(byte[] b, String key) {
        int n = b.length, offSize = b[n - 26] & 0xff, refSize = b[n - 25] & 0xff;
        long count = be(b, n - 24, 8), table = be(b, n - 8, 8);
        int top = (int) be(b, n - 16, 8);
        int dict = (int) be(b, (int) table + top * offSize, offSize);
        if ((b[dict] & 0xf0) != 0xd0) return null;
        int[] len = plistLength(b, dict);
        for (int i = 0; i < len[0]; i++) {
            int k = (int) be(b, len[1] + i * refSize, refSize), v = (int) be(b, len[1] + (len[0] + i) * refSize, refSize);
            if (k >= count || v >= count) return null;
            if (key.equals(plistString(b, (int) be(b, (int) table + k * offSize, offSize))))
                return plistString(b, (int) be(b, (int) table + v * offSize, offSize));
        }
        return null;
    }

    // An ASCII (0x5n) or UTF-16 (0x6n) string object, or null for other types
    private static String plistString(byte[] b, int at) {
        int type = b[at] & 0xf0;
        int[] len = plistLength(b, at);
        if (type == 0x50) return new String(b, len[1], len[0], StandardCharsets.US_ASCII);
        if (type == 0x60) return new String(b, len[1], len[0] * 2, StandardCharsets.UTF_16BE);
        return null;
    }

    // An object's length and where its contents start: the marker's low four bits,
    // or when those are all set, an integer object (0x1n, 2^n bytes) after it
    private static int[] plistLength(byte[] b, int at) {
        int low = b[at] & 0x0f;
        if (low != 0x0f) return new int[] {low, at + 1};
        int bytes = 1 << (b[at + 1] & 0x0f);
        return new int[] {(int) be(b, at + 2, bytes), at + 2 + bytes};
    }

    private static long be(byte[] b, int at, int size) {
        long v = 0;
        for (int i = 0; i < size; i++) v = v << 8 | (b[at + i] & 0xff);
        return v;
    }

    // Where the arm64 code starts: 0 in a thin executable, its slice in a fat one, or -1
    private static long arm64Slice(RandomAccessFile r) throws IOException {
        r.seek(0);
        if (r.readInt() != 0xCAFEBABE) return 0;
        int n = r.readInt();
        long slice = -1;
        for (int i = 0; i < n; i++) {
            int cpu = r.readInt();
            r.readInt();
            int off = r.readInt();
            r.readInt();
            r.readInt();
            if (cpu == 0x0100000C) slice = off & 0xffffffffL;
        }
        return slice;
    }

    // The command count followed by the load commands, or null when the slice isn't
    // 64-bit iOS code
    private static byte[] loadCommands(RandomAccessFile r, long slice) throws IOException {
        r.seek(slice);
        byte[] hdr = new byte[32];
        r.readFully(hdr);
        if (le32(hdr, 0) != 0xFEEDFACF || le32(hdr, 4) != 0x0100000C) return null;
        int ncmds = le32(hdr, 16), sizeofcmds = le32(hdr, 20);
        if (sizeofcmds < 0 || sizeofcmds > 1 << 20) return null;
        byte[] cmds = new byte[4 + sizeofcmds];
        System.arraycopy(hdr, 16, cmds, 0, 4);
        r.readFully(cmds, 4, sizeofcmds);
        return cmds;
    }

    private static int le32(byte[] b, int o) {
        return (b[o] & 0xff) | (b[o + 1] & 0xff) << 8 | (b[o + 2] & 0xff) << 16 | (b[o + 3] & 0xff) << 24;
    }

    private static boolean isAppFile(String zipName, String file) {
        // An .ipa has Payload/<Name>.app/<file>; also accept a zipped <Name>.app
        // folder (at any depth) or the two files zipped on their own.
        if (zipName.equals(file)) return true;
        if (!zipName.endsWith("/" + file)) return false;
        String dir = zipName.substring(0, zipName.length() - file.length() - 1);
        return dir.endsWith(".app") && !dir.contains("__MACOSX");
    }

    private static String findAppDir(ContentResolver cr, Uri tree, String dirId, int depth) {
        if (childId(cr, tree, dirId, "ftl.dat") != null && childId(cr, tree, dirId, "FTL") != null) return dirId;
        if (depth > 3) return null;
        Uri kids = DocumentsContract.buildChildDocumentsUriUsingTree(tree, dirId);
        try (Cursor c = cr.query(kids, new String[] {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_MIME_TYPE}, null, null, null)) {
            while (c != null && c.moveToNext()) {
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(1))) {
                    String r = findAppDir(cr, tree, c.getString(0), depth + 1);
                    if (r != null) return r;
                }
            }
        }
        return null;
    }

    private static String childId(ContentResolver cr, Uri tree, String dirId, String name) {
        Uri kids = DocumentsContract.buildChildDocumentsUriUsingTree(tree, dirId);
        try (Cursor c = cr.query(kids, new String[] {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME}, null, null, null)) {
            while (c != null && c.moveToNext())
                if (name.equals(c.getString(1))) return c.getString(0);
        }
        return null;
    }

    private static long querySize(ContentResolver cr, Uri uri) {
        try (Cursor c = cr.query(uri, new String[] {android.provider.OpenableColumns.SIZE}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getLong(0);
        } catch (Exception ignored) {
        }
        return -1;
    }

    private static void copyFile(File src, File dst, Progress p) throws IOException {
        try (InputStream in = new FileInputStream(src); OutputStream os = new FileOutputStream(dst)) {
            byte[] buf = new byte[1 << 16];
            long done = 0, total = src.length();
            int n;
            while ((n = in.read(buf)) > 0) {
                os.write(buf, 0, n);
                done += n;
                p.onProgress(dst.getName(), done, total);
            }
        }
    }

    static void deleteRecursive(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteRecursive(k);
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }

    private static final class CountingStream extends java.io.FilterInputStream {
        long count;

        CountingStream(InputStream in) {
            super(in);
        }

        @Override
        public int read() throws IOException {
            int r = super.read();
            if (r >= 0) count++;
            return r;
        }

        @Override
        public int read(byte[] b, int off, int len) throws IOException {
            int r = super.read(b, off, len);
            if (r > 0) count += r;
            return r;
        }
    }
}
