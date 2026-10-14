package dev.ftlport;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.net.Uri;
import android.provider.OpenableColumns;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Enumeration;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.TreeSet;
import java.util.zip.Deflater;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * Mods in the Slipstream format (.ftl, which is a zip) made for FTL 1.6 on
 * PC. The player's mods are kept in files/mods with a list of which are on and
 * in what order. Applying them merges every mod, in order, into one overlay
 * that the game reads in front of its own ftl.dat (native ux/mods.c):
 *  - name.xml.append (or name.append.xml): Slipstream's advanced XML, see ModXml
 *  - name.xml.rawappend / name.xml.rawclobber: added to / replaces the file as text
 *  - img/....png: converted to the texture format the iPad build uses (.tex)
 *  - anything else under audio/, data/, fonts/ or img/: replaces or adds that file
 */
final class Mods {
    interface Progress {
        void onProgress(String what, int done, int total);
    }

    static final class Mod {
        final String file;
        boolean on;

        Mod(String file, boolean on) {
            this.file = file;
            this.on = on;
        }
    }

    static final class Result {
        int files, images;
        final List<String> notes = new ArrayList<>();
    }

    private static final Set<String> FOLDERS = new TreeSet<>(java.util.Arrays.asList("audio", "data", "fonts", "img"));

    static File dir(Context ctx) {
        return new File(ctx.getFilesDir(), "mods");
    }

    private static File listFile(Context ctx) {
        return new File(dir(ctx), "list.txt");
    }

    private static File buildDir(Context ctx) {
        return new File(dir(ctx), "build");
    }

    /** The overlay index the game should load, or null when no mods are applied. */
    static File activeIndex(Context ctx) {
        File idx = new File(buildDir(ctx), "overlay.idx");
        return idx.isFile() && new File(buildDir(ctx), "overlay.dat").isFile() ? idx : null;
    }

    /** The mods in the applied overlay, in order (empty when none). */
    static List<String> applied(Context ctx) {
        List<String> out = new ArrayList<>();
        if (activeIndex(ctx) == null) return out;
        try (BufferedReader r = new BufferedReader(new FileReader(new File(buildDir(ctx), "applied.txt")))) {
            for (String line; (line = r.readLine()) != null; )
                if (!line.isEmpty()) out.add(line);
        } catch (IOException ignored) {
        }
        return out;
    }

    static List<Mod> load(Context ctx) {
        List<Mod> out = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new FileReader(listFile(ctx)))) {
            for (String line; (line = r.readLine()) != null; ) {
                int tab = line.indexOf('\t');
                if (tab < 0) continue;
                String file = line.substring(tab + 1);
                if (new File(dir(ctx), file).isFile()) out.add(new Mod(file, line.startsWith("1")));
            }
        } catch (IOException ignored) {
        }
        return out;
    }

    static void save(Context ctx, List<Mod> mods) throws IOException {
        StringBuilder sb = new StringBuilder();
        for (Mod m : mods) sb.append(m.on ? '1' : '0').append('\t').append(m.file).append('\n');
        writeAtomically(listFile(ctx), sb.toString().getBytes(StandardCharsets.UTF_8));
    }

    /** Copies a picked mod into the app, checks it looks like one and adds it to the list (switched on). */
    static String add(Context ctx, Uri uri) throws IOException {
        ContentResolver cr = ctx.getContentResolver();
        String name = displayName(cr, uri).replace('/', '_');
        File d = dir(ctx);
        //noinspection ResultOfMethodCallIgnored
        d.mkdirs();
        File tmp = new File(d, "incoming.tmp");
        try (InputStream in = cr.openInputStream(uri); OutputStream os = new FileOutputStream(tmp)) {
            if (in == null) throw new IOException(ctx.getString(R.string.mods_cant_open, name));
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0; ) os.write(buf, 0, n);
        }
        try (ZipFile z = new ZipFile(tmp)) {
            if (prefix(z) == null) throw new IOException(ctx.getString(R.string.mods_not_a_mod, name));
        } catch (IOException e) {
            //noinspection ResultOfMethodCallIgnored
            tmp.delete();
            throw e.getMessage() != null && e.getMessage().contains(name) ? e
                    : new IOException(ctx.getString(R.string.mods_not_a_mod, name));
        }
        List<Mod> mods = load(ctx);
        String file = name;
        for (int i = 2; new File(d, file).exists(); i++) file = i + " " + name;
        if (!tmp.renameTo(new File(d, file))) throw new IOException("Couldn't store " + name);
        mods.add(new Mod(file, true));
        save(ctx, mods);
        return file;
    }

    static void remove(Context ctx, String file) throws IOException {
        List<Mod> mods = load(ctx);
        mods.removeIf(m -> m.file.equals(file));
        save(ctx, mods);
        //noinspection ResultOfMethodCallIgnored
        new File(dir(ctx), file).delete();
    }

    /** Stops using mods (the files stay in the list). */
    static void turnOff(Context ctx) {
        deleteTree(buildDir(ctx));
    }

    // Files of a mod are relative to its root, which holds data/, img/ and so on.
    // Some mods are zipped with a folder around that: find the root.
    private static String prefix(ZipFile z) {
        String found = null;
        for (Enumeration<? extends ZipEntry> en = z.entries(); en.hasMoreElements(); ) {
            String n = clean(en.nextElement().getName());
            String[] parts = n.split("/");
            for (int i = 0; i < parts.length - 1 && i < 2; i++)
                if (FOLDERS.contains(parts[i].toLowerCase(Locale.ROOT))) {
                    StringBuilder p = new StringBuilder();
                    for (int j = 0; j < i; j++) p.append(parts[j]).append('/');
                    if (found == null || p.length() < found.length()) found = p.toString();
                    break;
                }
        }
        return found;
    }

    private static String clean(String name) {
        name = name.replace('\\', '/');
        while (name.startsWith("/") || name.startsWith("./")) name = name.substring(name.startsWith("/") ? 1 : 2);
        return name;
    }

    // ---------------------------------------------------------------- applying
    /** Merges the switched-on mods and makes them the ones the game uses. */
    static Result apply(Context ctx, List<Mod> mods, Progress progress) throws IOException, ModXml.ModException {
        Result res = new Result();
        List<Mod> on = new ArrayList<>();
        for (Mod m : mods)
            if (m.on) on.add(m);
        if (on.isEmpty()) {
            turnOff(ctx);
            return res;
        }
        try (Pkg pkg = new Pkg(new File(Importer.gameDir(ctx), "ftl.dat"))) {
            Overlay ov = new Overlay(pkg);
            int total = 0, done = 0;
            for (Mod m : on)
                try (ZipFile z = new ZipFile(new File(dir(ctx), m.file))) {
                    total += z.size();
                }
            for (Mod m : on) {
                try (ZipFile z = new ZipFile(new File(dir(ctx), m.file))) {
                    String root = prefix(z);
                    if (root == null) throw new IOException(ctx.getString(R.string.mods_not_a_mod, m.file));
                    List<? extends ZipEntry> entries = Collections.list(z.entries());
                    for (ZipEntry e : entries) {
                        progress.onProgress(m.file, ++done, total);
                        String name = clean(e.getName());
                        if (e.isDirectory() || !name.startsWith(root)) continue;
                        name = name.substring(root.length());
                        int slash = name.indexOf('/');
                        if (slash < 0 || !FOLDERS.contains(name.substring(0, slash).toLowerCase(Locale.ROOT))) continue;
                        byte[] body;
                        try (InputStream in = z.getInputStream(e)) {
                            body = readAll(in);
                        }
                        ov.add(m.file + ": " + name, name, body, res);
                    }
                }
            }
            ov.fixAtlases();
            write(ctx, ov, on, res);
        }
        return res;
    }

    private static final class Overlay {
        final Pkg pkg;
        final Map<String, byte[]> files = new TreeMap<>(String.CASE_INSENSITIVE_ORDER);
        final Map<String, ModXml.Doc> docs = new TreeMap<>(String.CASE_INSENSITIVE_ORDER);
        final Set<String> images = new TreeSet<>(String.CASE_INSENSITIVE_ORDER);  // replaced img/....png

        Overlay(Pkg pkg) {
            this.pkg = pkg;
        }

        // The game's spelling for files it has.
        String key(String name) {
            String k = pkg.name(name);
            return k != null ? k : name;
        }

        byte[] current(String name) throws IOException {
            ModXml.Doc d = docs.get(name);
            if (d != null) return ModXml.write(d).getBytes(StandardCharsets.UTF_8);
            byte[] b = files.get(name);
            return b != null ? b : pkg.read(name);
        }

        void put(String name, byte[] body) {
            docs.remove(name);
            files.put(key(name), body);
        }

        void add(String where, String name, byte[] body, Result res) throws IOException, ModXml.ModException {
            String lower = name.toLowerCase(Locale.ROOT);
            String target;
            if ((target = strip(name, lower, ".xml.append", ".append.xml")) != null) {
                ModXml.Doc d = docs.get(target);
                if (d == null) {
                    byte[] base = current(target);
                    d = ModXml.parse(base != null ? text(base) : "");
                    files.remove(target);
                    docs.put(key(target), d);
                }
                ModXml.append(d, text(body), where, res.notes);
            } else if ((target = strip(name, lower, ".xml.rawappend", ".rawappend.xml")) != null) {
                byte[] base = current(target);
                put(target, (base != null ? text(base) + "\r\n" + text(body) : text(body)).getBytes(StandardCharsets.UTF_8));
            } else if ((target = strip(name, lower, ".xml.rawclobber", ".rawclobber.xml")) != null) {
                put(target, body);
            } else if (lower.startsWith("img/") && lower.endsWith(".png")) {
                byte[] tex = texture(body);
                if (tex == null) {
                    res.notes.add(where + ": not a PNG image the app can read; skipped");
                    return;
                }
                put(name.substring(0, name.length() - 4) + ".tex", tex);
                images.add(name);
                res.images++;
            } else {
                put(name, body);
            }
        }

        private static String strip(String name, String lower, String suffix, String alt) {
            if (lower.endsWith(suffix)) return name.substring(0, name.length() - suffix.length()) + ".xml";
            if (lower.endsWith(alt)) return name.substring(0, name.length() - alt.length()) + ".xml";
            return null;
        }

        // The iPad build packs many small images into atlases (atlas/*.txt lists
        // "name x y w h"). A replaced image must come out of its atlas, or the
        // game would keep using the packed original.
        void fixAtlases() throws IOException {
            if (images.isEmpty()) return;
            for (String atlas : pkg.names("atlas/")) {
                if (!atlas.toLowerCase(Locale.ROOT).endsWith(".txt")) continue;
                String[] lines = text(current(atlas)).split("\n", -1);
                StringBuilder sb = new StringBuilder();
                boolean changed = false;
                for (String line : lines) {
                    String first = line.trim().split("\\s+", 2)[0];
                    if (!first.isEmpty() && images.contains(first)) {
                        changed = true;
                        continue;
                    }
                    if (sb.length() > 0) sb.append('\n');
                    sb.append(line);
                }
                if (changed) put(atlas, sb.toString().getBytes(StandardCharsets.UTF_8));
            }
        }
    }

    private static String text(byte[] b) {
        return new String(b, StandardCharsets.UTF_8);
    }

    /** A PNG as an uncompressed 32-bit .tex (BGRA, straight alpha), or null. */
    static byte[] texture(byte[] png) {
        BitmapFactory.Options o = new BitmapFactory.Options();
        o.inPremultiplied = false;
        o.inScaled = false;
        o.inPreferredConfig = Bitmap.Config.ARGB_8888;
        Bitmap b = BitmapFactory.decodeByteArray(png, 0, png.length, o);
        if (b == null) return null;
        int w = b.getWidth(), h = b.getHeight();
        if (w > 0xffff || h > 0xffff) return null;
        int[] px = new int[w * h];
        b.getPixels(px, 0, w, 0, 0, w, h);
        b.recycle();
        ByteBuffer out = ByteBuffer.allocate(32 + px.length * 4);
        out.putInt(0x5445580A);           // "TEX\n"
        out.put((byte) 2).put((byte) 8);  // version 2, 32-bit BGRA
        out.put((byte) 0).put((byte) 0);
        out.putShort((short) w).putShort((short) h);
        out.putInt(0x10000);              // scale 1.0 (16.16)
        out.putInt(32).putInt(px.length * 4);
        out.putInt(0).putInt(0);          // no hit mask
        for (int c : px) out.put((byte) c).put((byte) (c >> 8)).put((byte) (c >> 16)).put((byte) (c >>> 24));
        return out.array();
    }

    // Writes the overlay next to the current one, then swaps it in.
    private static void write(Context ctx, Overlay ov, List<Mod> on, Result res) throws IOException {
        for (Map.Entry<String, ModXml.Doc> e : ov.docs.entrySet())
            ov.files.put(e.getKey(), ModXml.write(e.getValue()).getBytes(StandardCharsets.UTF_8));
        File tmp = new File(dir(ctx), "build.tmp");
        deleteTree(tmp);
        if (!tmp.mkdirs()) throw new IOException("Couldn't create " + tmp);
        Deflater def = new Deflater(6);
        byte[] buf = new byte[1 << 16];
        try (OutputStream dat = new FileOutputStream(new File(tmp, "overlay.dat"));
             Writer idx = new OutputStreamWriter(new FileOutputStream(new File(tmp, "overlay.idx")),
                     StandardCharsets.UTF_8)) {
            long off = 0;
            for (Map.Entry<String, byte[]> e : ov.files.entrySet()) {
                byte[] body = e.getValue();
                def.reset();
                def.setInput(body);
                def.finish();
                ByteArrayOutputStream z = new ByteArrayOutputStream(body.length / 2 + 64);
                while (!def.finished()) z.write(buf, 0, def.deflate(buf));
                z.writeTo(dat);
                idx.write(off + "\t" + z.size() + "\t" + body.length + "\t" + (ov.pkg.has(e.getKey()) ? 0 : 1) + "\t"
                        + e.getKey() + "\n");
                off += z.size();
            }
        } finally {
            def.end();
        }
        StringBuilder names = new StringBuilder();
        for (Mod m : on) names.append(m.file).append('\n');
        writeAtomically(new File(tmp, "applied.txt"), names.toString().getBytes(StandardCharsets.UTF_8));
        File build = buildDir(ctx), old = new File(dir(ctx), "build.old");
        deleteTree(old);
        if (build.exists() && !build.renameTo(old)) throw new IOException("Couldn't replace the applied mods");
        if (!tmp.renameTo(build)) throw new IOException("Couldn't apply the mods");
        deleteTree(old);
        res.files = ov.files.size();
    }

    // ---------------------------------------------------------------- helpers
    private static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream bo = new ByteArrayOutputStream();
        byte[] buf = new byte[1 << 16];
        for (int n; (n = in.read(buf)) > 0; ) bo.write(buf, 0, n);
        return bo.toByteArray();
    }

    private static void writeAtomically(File f, byte[] data) throws IOException {
        File tmp = new File(f.getPath() + ".tmp");
        //noinspection ResultOfMethodCallIgnored
        f.getParentFile().mkdirs();
        try (FileOutputStream os = new FileOutputStream(tmp)) {
            os.write(data);
            os.getFD().sync();
        }
        if (!tmp.renameTo(f)) throw new IOException("Couldn't write " + f.getName());
    }

    static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null)
            for (File k : kids) deleteTree(k);
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }

    private static String displayName(ContentResolver cr, Uri uri) {
        try (Cursor c = cr.query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getString(0);
        } catch (Exception ignored) {
        }
        String p = uri.getLastPathSegment();
        return p != null ? p.substring(p.lastIndexOf('/') + 1) : "mod.ftl";
    }

    static long size(Context ctx, String file) {
        return new File(dir(ctx), file).length();
    }
}
