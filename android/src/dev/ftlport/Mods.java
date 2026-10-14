package dev.ftlport;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.ByteBuffer;
import java.nio.CharBuffer;
import java.nio.charset.Charset;
import java.nio.charset.CharsetDecoder;
import java.nio.charset.CoderResult;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Deque;
import java.util.Enumeration;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;
import java.util.zip.Deflater;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

/**
 * Mods made for FTL 1.6 on PC (.ftl files, which are zips). The player's mods
 * are kept in files/mods with a list of which are on and in what order.
 * Applying them runs ftlman, the mod manager also used on PC. It patches a
 * copy of the original ftl.dat (kept as ftl.dat.vanilla) in a staging folder,
 * and the result replaces the game's ftl.dat in one rename: a running game
 * keeps reading the file it opened, and if anything fails nothing changes.
 * The port then needs to know which images mods replaced (see native
 * ux/mods.c), so the atlas lists without those images go into a small overlay.
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

    private static final Set<String> FOLDERS = new TreeSet<>(Arrays.asList("audio", "data", "fonts", "img"));

    static File dir(Context ctx) {
        return new File(ctx.getFilesDir(), "mods");
    }

    private static File listFile(Context ctx) {
        return new File(dir(ctx), "list.txt");
    }

    private static File buildDir(Context ctx) {
        return new File(dir(ctx), "build");
    }

    private static File dat(Context ctx) {
        return new File(Importer.gameDir(ctx), "ftl.dat");
    }

    private static File vanilla(Context ctx) {
        return new File(Importer.gameDir(ctx), "ftl.dat.vanilla");
    }

    /** The port's overlay index for the applied mods, or null when no mods are applied. */
    static File activeIndex(Context ctx) {
        File idx = new File(buildDir(ctx), "overlay.idx");
        return idx.isFile() && new File(buildDir(ctx), "overlay.dat").isFile() ? idx : null;
    }

    /** The applied mods, in order (empty when none). */
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

    /** The picked file is the game itself (an .ipa, or a zip of FTL.app), not a mod. */
    static final class GameArchiveException extends IOException {
        GameArchiveException(String msg) {
            super(msg);
        }
    }

    /** A mod made for Hyperspace, which can't work here. */
    static final class HyperspaceException extends IOException {
        HyperspaceException(String msg) {
            super(msg);
        }
    }

    /** What adding a file did: the mods it added, and the ones it updated (same name as before). */
    static final class Added {
        final List<String> added = new ArrayList<>(), updated = new ArrayList<>();
    }

    /**
     * Copies a picked or opened file into the app and adds the mods in it to the
     * list, switched on. Besides a mod itself this takes a zip with mods inside
     * (as mods are often downloaded), and a mod with the same name as one in the
     * list replaces it, keeping its place.
     */
    static Added add(Context ctx, Uri uri) throws IOException {
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
        Added res = new Added();
        try (ZipFile z = open(ctx, tmp, name)) {
            if (prefix(z) != null) {
                z.close();
                store(ctx, tmp, name, true, res);
                return res;
            }
            // A download that wraps one or more mods: add each of them. With
            // several (often optional add-ons), leave the choice to the player.
            List<ZipEntry> inner = new ArrayList<>();
            for (Enumeration<? extends ZipEntry> en = z.entries(); en.hasMoreElements(); ) {
                ZipEntry e = en.nextElement();
                String n = clean(e.getName()).toLowerCase(Locale.ROOT);
                if (!e.isDirectory() && !n.contains("__macosx") && (n.endsWith(".ftl") || n.endsWith(".zip"))) inner.add(e);
            }
            if (inner.isEmpty()) {
                if (isGame(z)) throw new GameArchiveException(ctx.getString(R.string.mods_is_game, name));
                throw new IOException(ctx.getString(R.string.mods_not_a_mod, name));
            }
            for (ZipEntry e : inner) {
                String innerName = clean(e.getName());
                innerName = innerName.substring(innerName.lastIndexOf('/') + 1);
                File part = new File(d, "incoming-part.tmp");
                try (InputStream in = z.getInputStream(e); OutputStream os = new FileOutputStream(part)) {
                    byte[] buf = new byte[1 << 16];
                    for (int n; (n = in.read(buf)) > 0; ) os.write(buf, 0, n);
                }
                boolean mod;
                try (ZipFile pz = open(ctx, part, innerName)) {
                    mod = prefix(pz) != null;
                } catch (HyperspaceException ex) {
                    throw ex;
                } catch (IOException ex) {
                    mod = false;
                }
                if (mod) store(ctx, part, innerName, inner.size() == 1, res);
                //noinspection ResultOfMethodCallIgnored
                part.delete();
            }
            if (res.added.isEmpty() && res.updated.isEmpty())
                throw new IOException(ctx.getString(R.string.mods_not_a_mod, name));
            return res;
        } finally {
            //noinspection ResultOfMethodCallIgnored
            tmp.delete();
        }
    }

    // Opens a zip that should be a mod, refusing mods made for Hyperspace.
    private static ZipFile open(Context ctx, File f, String name) throws IOException {
        ZipFile z;
        try {
            z = new ZipFile(f);
        } catch (IOException e) {
            throw new IOException(ctx.getString(R.string.mods_not_a_mod, name));
        }
        if (needsHyperspace(z)) {
            z.close();
            throw new HyperspaceException(ctx.getString(R.string.mods_hyperspace, name));
        }
        return z;
    }

    // Hyperspace mods carry its settings file, data/hyperspace.xml (or an append to it).
    private static boolean needsHyperspace(ZipFile z) {
        for (Enumeration<? extends ZipEntry> en = z.entries(); en.hasMoreElements(); ) {
            String n = clean(en.nextElement().getName()).toLowerCase(Locale.ROOT);
            if (n.endsWith("data/hyperspace.xml") || n.contains("data/hyperspace.xml.")
                    || n.endsWith("data/hyperspace.append.xml")) return true;
        }
        return false;
    }

    // FTL.app with the game's data in it: an .ipa or a zipped app.
    private static boolean isGame(ZipFile z) {
        for (Enumeration<? extends ZipEntry> en = z.entries(); en.hasMoreElements(); ) {
            String n = clean(en.nextElement().getName()).toLowerCase(Locale.ROOT);
            if (n.endsWith(".app/ftl.dat")) return true;
        }
        return false;
    }

    // Moves a checked mod into place: a new entry, or a newer copy of one with the
    // same name. A mod zipped with a folder around its files is unwrapped, since
    // mod managers expect data/, img/ and so on at the top.
    private static void store(Context ctx, File f, String name, boolean on, Added res) throws IOException {
        try (ZipFile z = new ZipFile(f)) {
            String root = prefix(z);
            if (root != null && !root.isEmpty()) {
                File flat = new File(f.getPath() + ".flat");
                unwrap(z, root, flat);
                z.close();
                if (!f.delete() || !flat.renameTo(f)) throw new IOException("Couldn't store " + name);
            }
        }
        List<Mod> mods = load(ctx);
        File target = new File(dir(ctx), name);
        boolean known = false;
        for (Mod m : mods)
            if (m.file.equals(name)) known = true;
        if (target.exists() && !known) {  // a stray file left behind: replace it
            //noinspection ResultOfMethodCallIgnored
            target.delete();
        }
        if (target.exists() && !target.delete() || !f.renameTo(target))
            throw new IOException("Couldn't store " + name);
        if (known) {
            res.updated.add(name);
        } else {
            mods.add(new Mod(name, on));
            res.added.add(name);
        }
        save(ctx, mods);
    }

    private static void unwrap(ZipFile z, String root, File out) throws IOException {
        byte[] buf = new byte[1 << 16];
        try (ZipOutputStream zo = new ZipOutputStream(new FileOutputStream(out))) {
            for (Enumeration<? extends ZipEntry> en = z.entries(); en.hasMoreElements(); ) {
                ZipEntry e = en.nextElement();
                String n = clean(e.getName());
                if (e.isDirectory() || !n.startsWith(root) || n.length() == root.length()) continue;
                zo.putNextEntry(new ZipEntry(n.substring(root.length())));
                try (InputStream in = z.getInputStream(e)) {
                    for (int r; (r = in.read(buf)) > 0; ) zo.write(buf, 0, r);
                }
                zo.closeEntry();
            }
        }
    }

    static boolean isIpa(Context ctx, Uri uri) {
        return displayName(ctx.getContentResolver(), uri).toLowerCase(Locale.ROOT).endsWith(".ipa");
    }

    static void remove(Context ctx, String file) throws IOException {
        List<Mod> mods = load(ctx);
        mods.removeIf(m -> m.file.equals(file));
        save(ctx, mods);
        //noinspection ResultOfMethodCallIgnored
        new File(dir(ctx), file).delete();
    }

    /**
     * Stops using mods (the files stay in the list): puts the original ftl.dat
     * back. Also called when the game files are imported again, which leaves
     * nothing to restore.
     */
    static void turnOff(Context ctx) {
        File vanilla = vanilla(ctx);
        if (vanilla.isFile() && !vanilla.renameTo(dat(ctx)))
            android.util.Log.e("FTLPort", "mods: couldn't restore ftl.dat");
        deleteTree(buildDir(ctx));
        deleteTree(stageDir(ctx));
        markChanged(ctx);
    }

    private static File stageDir(Context ctx) {
        return new File(dir(ctx), "stage");
    }

    // A run saved with other mods may not load, so after a change the game
    // starts at the title screen instead of continuing (see GameActivity).
    private static File changedMarker(Context ctx) {
        return new File(dir(ctx), "changed");
    }

    private static void markChanged(Context ctx) {
        try {
            //noinspection ResultOfMethodCallIgnored
            dir(ctx).mkdirs();
            //noinspection ResultOfMethodCallIgnored
            changedMarker(ctx).createNewFile();
        } catch (IOException ignored) {
        }
    }

    /** True once after the mods changed. */
    static boolean takeChanged(Context ctx) {
        return changedMarker(ctx).delete();
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
    /** Patches the game with the switched-on mods, in order, using ftlman. */
    static void apply(Context ctx, List<Mod> mods, Progress progress) throws IOException {
        List<Mod> on = new ArrayList<>();
        for (Mod m : mods)
            if (m.on) on.add(m);
        if (on.isEmpty()) {
            turnOff(ctx);
            return;
        }
        // Refuse Hyperspace mods added before that check existed, and make sure
        // there's room: the patched copy (and the original, the first time).
        File vanilla = vanilla(ctx), stage = stageDir(ctx);
        long need = (64L << 20) + (vanilla.isFile() ? vanilla : dat(ctx)).length();
        if (!vanilla.isFile()) need += dat(ctx).length();
        for (Mod m : on) {
            File f = new File(dir(ctx), m.file);
            try (ZipFile z = open(ctx, f, m.file)) {
                need += f.length() * 2;
            }
        }
        long free = Importer.gameDir(ctx).getUsableSpace();
        if (free < need)
            throw new IOException(ctx.getString(R.string.mods_no_space, (need - free + (1 << 20) - 1) >> 20));
        List<String> notes = new ArrayList<>();
        for (Mod m : on) fixTextEncoding(new File(dir(ctx), m.file), m.file, notes);

        boolean swapped = false;
        try {
            if (!vanilla.isFile()) {  // first time: keep the original
                File tmp = new File(vanilla.getPath() + ".tmp");
                copy(dat(ctx), tmp);
                if (!tmp.renameTo(vanilla)) throw new IOException("Couldn't keep a copy of ftl.dat");
            }
            deleteTree(stage);
            if (!stage.mkdirs()) throw new IOException("Couldn't create " + stage);
            // ftlman patches <dir>/ftl.dat, starting from <dir>/ftl.dat.vanilla.
            android.system.Os.symlink(vanilla.getPath(), new File(stage, "ftl.dat.vanilla").getPath());
            runFtlman(ctx, stage, on, progress, notes);
            File patched = new File(stage, "ftl.dat");
            writeOverlay(ctx, patched, on);
            if (!patched.renameTo(dat(ctx))) throw new IOException("Couldn't install the patched ftl.dat");
            swapped = true;
            File build = buildDir(ctx), old = new File(dir(ctx), "build.old"), next = new File(dir(ctx), "build.tmp");
            deleteTree(old);
            if (build.exists() && !build.renameTo(old)) throw new IOException("Couldn't replace the applied mods");
            if (!next.renameTo(build)) throw new IOException("Couldn't apply the mods");
            deleteTree(old);
        } catch (android.system.ErrnoException e) {
            throw new IOException(e.getMessage());
        } catch (IOException | RuntimeException e) {
            if (swapped) turnOff(ctx);  // don't leave a patched ftl.dat without its overlay
            throw e;
        } finally {
            deleteTree(stage);
            deleteTree(new File(dir(ctx), "build.tmp"));
        }
        markChanged(ctx);
    }

    private static void copy(File from, File to) throws IOException {
        try (InputStream in = new java.io.FileInputStream(from); FileOutputStream out = new FileOutputStream(to)) {
            byte[] buf = new byte[1 << 20];
            for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
            out.getFD().sync();
        }
    }

    // Text files in mods are read by ftlman as UTF-8 (or UTF-16 with a byte order
    // mark). Slipstream reads files that aren't UTF-8 as Windows-1252, and some
    // older mods were saved that way, so convert them the first time they're
    // applied: valid UTF-8 stays as it is, other bytes are read as Windows-1252.
    private static void fixTextEncoding(File mod, String name, List<String> notes) throws IOException {
        List<String> bad = new ArrayList<>();
        try (ZipFile z = new ZipFile(mod)) {
            for (Enumeration<? extends ZipEntry> en = z.entries(); en.hasMoreElements(); ) {
                ZipEntry e = en.nextElement();
                if (!e.isDirectory() && isText(e.getName())) {
                    try (InputStream in = z.getInputStream(e)) {
                        if (toUtf8(readAll(in)) != null) bad.add(e.getName());
                    }
                }
            }
            if (bad.isEmpty()) return;
            File tmp = new File(mod.getPath() + ".tmp");
            byte[] buf = new byte[1 << 16];
            try (ZipOutputStream zo = new ZipOutputStream(new FileOutputStream(tmp))) {
                for (Enumeration<? extends ZipEntry> en = z.entries(); en.hasMoreElements(); ) {
                    ZipEntry e = en.nextElement();
                    zo.putNextEntry(new ZipEntry(e.getName()));
                    try (InputStream in = z.getInputStream(e)) {
                        if (bad.contains(e.getName())) {
                            zo.write(toUtf8(readAll(in)));
                        } else {
                            for (int n; (n = in.read(buf)) > 0; ) zo.write(buf, 0, n);
                        }
                    }
                    zo.closeEntry();
                }
            }
            if (!tmp.renameTo(mod)) throw new IOException("Couldn't update " + name);
        }
        for (String b : bad) notes.add(name + ": " + b + " converted to UTF-8 (it was Windows-1252)");
    }

    private static boolean isText(String path) {
        String n = path.toLowerCase(Locale.ROOT);
        for (String ext : new String[] {".xml", ".txt", ".append", ".rawappend", ".rawclobber", ".lua"})
            if (n.endsWith(ext)) return true;
        return false;
    }

    /** The text as UTF-8, or null if it already is (or is UTF-16 with a byte order mark). */
    static byte[] toUtf8(byte[] b) {
        int b0 = b.length > 0 ? b[0] & 0xff : -1, b1 = b.length > 1 ? b[1] & 0xff : -1;
        if ((b0 == 0xff && b1 == 0xfe) || (b0 == 0xfe && b1 == 0xff)) return null;
        CharsetDecoder utf8 = StandardCharsets.UTF_8.newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT);
        Charset cp1252 = Charset.forName("windows-1252");
        ByteBuffer in = ByteBuffer.wrap(b);
        CharBuffer out = CharBuffer.allocate(b.length);  // never more characters than bytes
        boolean changed = false;
        for (;;) {
            CoderResult r = utf8.decode(in, out, true);
            if (r.isUnderflow()) break;
            if (!r.isError()) throw new IllegalStateException(r.toString());
            byte[] odd = new byte[r.length()];
            in.get(odd);
            out.put(new String(odd, cp1252));
            changed = true;
        }
        if (!changed) return null;
        utf8.flush(out);
        out.flip();
        ByteBuffer enc = StandardCharsets.UTF_8.encode(out);
        byte[] res = new byte[enc.remaining()];
        enc.get(res);
        return res;
    }

    private static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream bo = new ByteArrayOutputStream();
        byte[] buf = new byte[1 << 16];
        for (int n; (n = in.read(buf)) > 0; ) bo.write(buf, 0, n);
        return bo.toByteArray();
    }

    // Its warnings (often leftovers that mods carry for older versions of FTL)
    // only go to the log, as with ftlman on PC.
    private static void runFtlman(Context ctx, File dataDir, List<Mod> on, Progress progress, List<String> notes)
            throws IOException {
        File exe = new File(ctx.getApplicationInfo().nativeLibraryDir, "libftlman.so");
        List<String> cmd = new ArrayList<>(Arrays.asList(exe.getPath(), "patch", "--data-dir", dataDir.getPath()));
        for (Mod m : on) cmd.add(new File(dir(ctx), m.file).getPath());
        ProcessBuilder pb = new ProcessBuilder(cmd).redirectErrorStream(true);
        // ftlman keeps its settings in the usual per-user folders: give it some of ours.
        File home = new File(ctx.getFilesDir(), "ftlman");
        Map<String, String> env = pb.environment();
        env.put("HOME", home.getPath());
        env.put("XDG_CONFIG_HOME", new File(home, "config").getPath());
        env.put("XDG_DATA_HOME", new File(home, "data").getPath());
        env.put("XDG_CACHE_HOME", new File(ctx.getCacheDir(), "ftlman").getPath());
        env.put("TMPDIR", ctx.getCacheDir().getPath());
        env.put("RUST_LOG", "info");
        env.put("NO_COLOR", "1");
        //noinspection ResultOfMethodCallIgnored
        home.mkdirs();

        Process p = pb.start();
        Deque<String> tail = new ArrayDeque<>();
        int applied = 0;
        File log = new File(Logs.dir(ctx), "ftlman-last-run.txt");  // goes into Save logs
        //noinspection ResultOfMethodCallIgnored
        log.getParentFile().mkdirs();
        try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream(), StandardCharsets.UTF_8));
             Writer w = new OutputStreamWriter(new FileOutputStream(log), StandardCharsets.UTF_8)) {
            for (String n : notes) w.write(n + "\n");
            w.write(String.join(" ", cmd) + "\n");
            for (String line; (line = r.readLine()) != null; ) {
                line = line.replaceAll("\u001b\\[[0-9;]*m", "").trim();  // no colours, just in case
                if (line.isEmpty()) continue;
                w.write(line + "\n");
                if (tail.size() == 20) tail.removeFirst();
                tail.addLast(line);
                if (line.contains("Applying mod ")) {
                    progress.onProgress(line.substring(line.indexOf("Applying mod ") + 13), applied, on.size());
                    applied++;
                }
            }
        }
        int code;
        try {
            code = p.waitFor();
        } catch (InterruptedException e) {
            p.destroy();
            throw new IOException("Interrupted");
        }
        if (code != 0) {
            StringBuilder sb = new StringBuilder();
            for (String l : tail)
                if (l.startsWith("[ERROR]"))
                    sb.append(l.substring(7).trim().replaceFirst("^#\\d+:\\s*", "")).append('\n');
            throw new IOException(sb.length() > 0 ? sb.toString().trim() : "ftlman stopped with code " + code);
        }
    }

    // The atlas lists without the images mods replaced, so the game loads the
    // mods' files instead of the packed originals. Goes to build.tmp.
    private static void writeOverlay(Context ctx, File patched, List<Mod> on) throws IOException {
        File tmp = new File(dir(ctx), "build.tmp");
        deleteTree(tmp);
        if (!tmp.mkdirs()) throw new IOException("Couldn't create " + tmp);
        try (Pkg pkg = new Pkg(patched);
             OutputStream dat = new FileOutputStream(new File(tmp, "overlay.dat"));
             Writer idx = new OutputStreamWriter(new FileOutputStream(new File(tmp, "overlay.idx")),
                     StandardCharsets.UTF_8)) {
            Set<String> images = new TreeSet<>(String.CASE_INSENSITIVE_ORDER);
            for (String n : pkg.names("img/"))
                if (n.toLowerCase(Locale.ROOT).endsWith(".png")) images.add(n);
            long off = 0;
            Deflater def = new Deflater(6);
            byte[] buf = new byte[1 << 16];
            try {
                for (String atlas : pkg.names("atlas/")) {
                    if (images.isEmpty() || !atlas.toLowerCase(Locale.ROOT).endsWith(".txt")) continue;
                    String text = new String(pkg.read(atlas), StandardCharsets.UTF_8);
                    StringBuilder sb = new StringBuilder();
                    boolean changed = false;
                    for (String line : text.split("\n", -1)) {
                        String first = line.trim().split("\\s+", 2)[0];
                        if (!first.isEmpty() && images.contains(first)) {
                            changed = true;
                            continue;
                        }
                        if (sb.length() > 0) sb.append('\n');
                        sb.append(line);
                    }
                    if (!changed) continue;
                    byte[] body = sb.toString().getBytes(StandardCharsets.UTF_8);
                    def.reset();
                    def.setInput(body);
                    def.finish();
                    ByteArrayOutputStream z = new ByteArrayOutputStream(body.length / 2 + 64);
                    while (!def.finished()) z.write(buf, 0, def.deflate(buf));
                    z.writeTo(dat);
                    idx.write(off + "\t" + z.size() + "\t" + body.length + "\t0\t" + atlas + "\n");
                    off += z.size();
                }
            } finally {
                def.end();
            }
        }
        StringBuilder names = new StringBuilder();
        for (Mod m : on) names.append(m.file).append('\n');
        writeAtomically(new File(tmp, "applied.txt"), names.toString().getBytes(StandardCharsets.UTF_8));
    }

    // ---------------------------------------------------------------- helpers
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

    static String displayName(ContentResolver cr, Uri uri) {
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
