package dev.ftlport;

import java.io.ByteArrayOutputStream;
import java.io.Closeable;
import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;
import java.util.zip.DataFormatException;
import java.util.zip.Inflater;

/**
 * Reads the game's ftl.dat: a "PKG\n" archive (big-endian) with a 16-byte
 * header, 20-byte index entries (name hash, flags and path offset, data offset,
 * stored size, real size), a path table, then the files, zlib-compressed when
 * flag 1 is set. Lookups ignore case, like the game's.
 */
final class Pkg implements Closeable {
    private static final class Entry {
        final String name;
        final long off;
        final int size, len;
        final boolean deflated;

        Entry(String name, long off, int size, int len, boolean deflated) {
            this.name = name;
            this.off = off;
            this.size = size;
            this.len = len;
            this.deflated = deflated;
        }
    }

    private final RandomAccessFile mFile;
    private final Map<String, Entry> mEntries = new TreeMap<>(String.CASE_INSENSITIVE_ORDER);

    Pkg(File f) throws IOException {
        mFile = new RandomAccessFile(f, "r");
        byte[] head = new byte[16];
        mFile.readFully(head);
        ByteBuffer h = ByteBuffer.wrap(head);
        if (h.getInt() != 0x504B470A || h.getShort() != 16 || h.getShort() != 20)
            throw new IOException("ftl.dat isn't in the expected format");
        int count = h.getInt(), pathSize = h.getInt();
        byte[] index = new byte[count * 20], paths = new byte[pathSize];
        mFile.readFully(index);
        mFile.readFully(paths);
        ByteBuffer ix = ByteBuffer.wrap(index);
        for (int i = 0; i < count; i++) {
            ix.getInt();  // name hash
            int flagsAndPath = ix.getInt();
            long off = ix.getInt() & 0xffffffffL;
            int size = ix.getInt(), len = ix.getInt();
            int p = flagsAndPath & 0xffffff, e = p;
            while (e < pathSize && paths[e] != 0) e++;
            String name = new String(paths, p, e - p, StandardCharsets.UTF_8);
            mEntries.put(name, new Entry(name, off, size, len, (flagsAndPath >>> 24 & 1) != 0));
        }
    }

    boolean has(String path) {
        return mEntries.containsKey(path);
    }

    /** The name as the archive spells it, or null. */
    String name(String path) {
        Entry e = mEntries.get(path);
        return e != null ? e.name : null;
    }

    List<String> names(String prefix) {
        List<String> out = new ArrayList<>();
        String p = prefix.toLowerCase(Locale.ROOT);
        for (String n : mEntries.keySet())
            if (n.toLowerCase(Locale.ROOT).startsWith(p)) out.add(n);
        return out;
    }

    /** A file's contents, or null if there's no such file. */
    synchronized byte[] read(String path) throws IOException {
        Entry e = mEntries.get(path);
        if (e == null) return null;
        byte[] raw = new byte[e.size];
        mFile.seek(e.off);
        mFile.readFully(raw);
        if (!e.deflated) return raw;
        Inflater inf = new Inflater();
        try {
            inf.setInput(raw);
            ByteArrayOutputStream out = new ByteArrayOutputStream(Math.max(e.len, 64));
            byte[] buf = new byte[1 << 16];
            while (!inf.finished()) {
                int n = inf.inflate(buf);
                if (n == 0 && (inf.needsInput() || inf.needsDictionary())) break;
                out.write(buf, 0, n);
            }
            return out.toByteArray();
        } catch (DataFormatException ex) {
            throw new IOException("ftl.dat: " + path + " is damaged");
        } finally {
            inf.end();
        }
    }

    @Override
    public void close() throws IOException {
        mFile.close();
    }
}
