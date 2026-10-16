package dev.ftlport;

import android.content.Context;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;

/**
 * Repairs saves from the 0.3.2-hyperspace pre-release, once. Its Hyperspace kept its
 * profile in the game's prof.sav and its run in continue.sav, instead of files of its own
 * (hs_<mod>_prof.sav, hs_<mod>_continue.sav) as Hyperspace does now. FTL without
 * Hyperspace hangs reading such a prof.sav. This copies the profile and run to the mod's
 * files and takes Hyperspace's part out of prof.sav. It can go once nobody has saves from
 * that pre-release.
 */
final class OldSaves {
    private static final String TAG = "FTLPort";

    static void repair(Context ctx) {
        try {
            File docs = SaveTransfer.docsDir(ctx);
            File prof = new File(docs, SaveTransfer.PROFILE);
            if (prof.isFile()) {
                byte[] data = read(prof);
                int[] part = hyperspacePart(data);
                if (part != null) {
                    String prefix = lastPrefix(docs);
                    File own = new File(docs, prefix + "_prof.sav");
                    if (!own.exists()) write(own, data, 0, data.length);
                    File run = new File(docs, SaveTransfer.RUN), ownRun = new File(docs, prefix + "_continue.sav");
                    if (run.isFile() && !ownRun.exists()) {
                        byte[] r = read(run);
                        write(ownRun, r, 0, r.length);
                    }
                    byte[] plain = new byte[data.length - (part[1] - part[0])];
                    System.arraycopy(data, 0, plain, 0, part[0]);
                    System.arraycopy(data, part[1], plain, part[0], data.length - part[1]);
                    write(prof, plain, 0, plain.length);
                    Log.i(TAG, "saves: moved Hyperspace's profile out of prof.sav to " + own.getName());
                }
            }
            // Hyperspace reads its profile the way its version file says: without the profile
            // that file went with, it would read the game's as its own
            String[] names = docs.list();
            if (names != null)
                for (String n : names)
                    if (n.startsWith("hs") && n.endsWith("_version.sav")) {
                        String prefix = n.substring(0, n.length() - "_version.sav".length());
                        if (!new File(docs, prefix + "_prof.sav").exists() && new File(docs, n).delete())
                            Log.i(TAG, "saves: removed " + n + ", which had no profile");
                    }
        } catch (Exception e) {
            Log.w(TAG, "saves: couldn't check for pre-release Hyperspace saves", e);
        }
    }

    // ScoreKeeper::Save writes <prefix>_version.sav right before the profile, so the
    // newest one names the mod that wrote prof.sav last
    private static String lastPrefix(File docs) {
        String best = "hs";
        long bestTime = Long.MIN_VALUE;
        String[] names = docs.list();
        if (names != null)
            for (String n : names)
                if (n.startsWith("hs") && n.endsWith("_version.sav")) {
                    long t = new File(docs, n).lastModified();
                    if (t >= bestTime) {
                        best = n.substring(0, n.length() - "_version.sav".length());
                        bestTime = t;
                    }
                }
        return best;
    }

    // Reads a profile like the game's FileHelper, without trusting it
    private static final class Reader {
        final ByteBuffer b;
        boolean ok = true;

        Reader(byte[] data, int pos) {
            b = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
            b.position(pos);
        }

        Reader copy() {
            Reader r = new Reader(b.array(), b.position());
            r.ok = ok;
            return r;
        }

        int i32() {
            if (b.remaining() < 4) {
                ok = false;
                return 0;
            }
            return b.getInt();
        }

        int count() {
            int n = i32();
            if (n < 0 || n > 100000) ok = false;
            return ok ? n : 0;
        }

        String str() {
            int n = count();
            if (!ok || b.remaining() < n) {
                ok = false;
                return "";
            }
            String s = new String(b.array(), b.position(), n, StandardCharsets.UTF_8);
            b.position(b.position() + n);
            return s;
        }

        void pairs(int n) {
            while (ok && n-- > 0) {
                str();
                i32();
            }
        }

        void scores(int n) {
            while (ok && n-- > 0) {
                str();
                str();
                for (int i = 0; i < 5; i++) i32();
            }
        }
    }

    // What follows the achievements in the game's own profile (ScoreKeeper::Save): ship
    // unlocks, top scores, ship scores, stats, games and victories, crew records
    private static boolean gameTailFits(Reader r) {
        for (int i = 0; i < 24; i++) r.i32();
        r.scores(r.count());
        r.scores(r.count());
        for (int i = 0; i < 10; i++) r.i32();
        for (int i = 0; i < 5 && r.ok; i++) {
            r.i32();
            r.str();
            r.str();
            r.i32();
        }
        return r.ok && r.b.remaining() == 0;
    }

    // Where Hyperspace's part (AchievementTracker::SaveProfile's hook) is in a profile:
    // {start, end}, or null if it has none
    static int[] hyperspacePart(byte[] data) {
        Reader r = new Reader(data, 8);  // after the format and firstTimeShips
        for (int n = r.count(); r.ok && n > 0; n--) {
            String name = r.str();
            r.i32();
            if (name.contains("_VICTORY")) {
                r.i32();
                r.i32();
                r.i32();
            }
        }
        if (!r.ok || gameTailFits(r.copy())) return null;
        int start = r.b.position();
        for (int n = r.count(); r.ok && n > 0; n--) r.str();      // unlocked ships
        r.pairs(r.count());                                         // quest ships
        r.pairs(r.count());                                         // ship victories
        for (int n = r.count(); r.ok && n > 0; n--) {               // victory types
            r.str();
            r.pairs(r.count());
        }
        r.pairs(r.count());                                         // achievements
        for (int n = r.count(); r.ok && n > 0; n--) {               // ship scores
            r.str();
            r.scores(r.count());
        }
        r.pairs(r.count());                                         // meta variables
        int end = r.b.position();
        return r.ok && gameTailFits(r.copy()) ? new int[] {start, end} : null;
    }

    private static byte[] read(File f) throws IOException {
        try (InputStream in = new FileInputStream(f)) {
            byte[] out = new byte[(int) f.length()];
            int off = 0, n;
            while (off < out.length && (n = in.read(out, off, out.length - off)) > 0) off += n;
            if (off != out.length) throw new IOException("Short read of " + f);
            return out;
        }
    }

    private static void write(File f, byte[] data, int off, int len) throws IOException {
        File tmp = new File(f.getPath() + ".tmp");
        try (OutputStream os = new FileOutputStream(tmp)) {
            os.write(data, off, len);
            ((FileOutputStream) os).getFD().sync();
        }
        if (!tmp.renameTo(f)) throw new IOException("Couldn't write " + f);
    }

    private OldSaves() {}
}
