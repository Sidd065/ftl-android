package dev.ftlport;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Deque;
import java.util.IdentityHashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * FTL's data files, and the mods made for them, are XML that's often not
 * quite well-formed: several top-level tags, stray closing tags, bare
 * ampersands. They're parsed leniently here, patched the way the Slipstream
 * mod manager does it (".xml.append" files with mod:find... tags) and written
 * back out with text and attribute values exactly as they were.
 */
final class ModXml {
    static class ModException extends Exception {
        ModException(String msg) {
            super(msg);
        }
    }

    abstract static class Node {
        abstract Node copy();
    }

    /** Text (when {@code text}), or a comment, declaration or CDATA section, kept as written. */
    static final class Raw extends Node {
        final String raw;
        final boolean text;

        Raw(String raw, boolean text) {
            this.raw = raw;
            this.text = text;
        }

        @Override
        Node copy() {
            return this;  // immutable
        }
    }

    static final class Element extends Node {
        String name;
        final Map<String, String> attrs = new LinkedHashMap<>();  // raw values
        final List<Node> kids;

        Element(String name) {
            this(name, new ArrayList<>());
        }

        Element(String name, List<Node> kids) {
            this.name = name;
            this.kids = kids;
        }

        @Override
        Element copy() {
            return copyAs(name);
        }

        Element copyAs(String newName) {
            Element e = new Element(newName);
            e.attrs.putAll(attrs);
            for (Node k : kids) e.kids.add(k.copy());
            return e;
        }

        List<Element> elements() {
            List<Element> out = new ArrayList<>();
            for (Node k : kids)
                if (k instanceof Element) out.add((Element) k);
            return out;
        }

        Element child(String name) {
            for (Node k : kids)
                if (k instanceof Element && ((Element) k).name.equals(name)) return (Element) k;
            return null;
        }

        /** The text directly inside, entities decoded. */
        String text() {
            StringBuilder sb = new StringBuilder();
            for (Node k : kids)
                if (k instanceof Raw && ((Raw) k).text) sb.append(((Raw) k).raw);
            return decode(sb.toString());
        }
    }

    static final class Doc {
        final List<Node> top = new ArrayList<>();
    }

    // ---------------------------------------------------------------- parsing
    static Doc parse(String s) {
        Doc d = new Doc();
        Deque<Element> open = new ArrayDeque<>();
        int n = s.length(), i = n > 0 && s.charAt(0) == '﻿' ? 1 : 0;
        while (i < n) {
            int lt = s.indexOf('<', i);
            if (lt < 0) lt = n;
            if (lt > i) add(d, open, new Raw(s.substring(i, lt), true));
            if (lt >= n) break;
            if (s.startsWith("<!--", lt)) {
                i = skipPast(s, lt + 4, "-->");
                add(d, open, new Raw(s.substring(lt, i), false));
            } else if (s.startsWith("<![CDATA[", lt)) {
                i = skipPast(s, lt + 9, "]]>");
                add(d, open, new Raw(s.substring(lt, i), true));
            } else if (s.startsWith("<?", lt) || s.startsWith("<!", lt)) {
                i = skipPast(s, lt + 2, ">");
                add(d, open, new Raw(s.substring(lt, i), false));
            } else if (s.startsWith("</", lt)) {
                i = skipPast(s, lt + 2, ">");
                close(open, s.substring(lt + 2, i - (s.charAt(i - 1) == '>' ? 1 : 0)).trim());
            } else {
                i = startTag(s, lt, d, open);
            }
        }
        return d;
    }

    private static int skipPast(String s, int from, String end) {
        int e = s.indexOf(end, from);
        return e < 0 ? s.length() : e + end.length();
    }

    private static boolean space(char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    }

    private static int startTag(String s, int lt, Doc d, Deque<Element> open) {
        int n = s.length(), p = lt + 1;
        while (p < n && !space(s.charAt(p)) && s.charAt(p) != '>' && s.charAt(p) != '/') p++;
        if (p == lt + 1) {  // a lone '<'
            add(d, open, new Raw("<", true));
            return lt + 1;
        }
        Element el = new Element(s.substring(lt + 1, p));
        boolean empty = false;
        while (p < n) {
            char c = s.charAt(p);
            if (space(c)) {
                p++;
            } else if (c == '>') {
                p++;
                break;
            } else if (c == '/') {
                p++;
                if (p < n && s.charAt(p) == '>') {
                    empty = true;
                    p++;
                    break;
                }
            } else {
                int a = p;
                while (p < n && !space(s.charAt(p)) && "=>/".indexOf(s.charAt(p)) < 0) p++;
                String name = s.substring(a, p), value = "";
                int q = p;
                while (q < n && space(s.charAt(q))) q++;
                if (q < n && s.charAt(q) == '=') {
                    q++;
                    while (q < n && space(s.charAt(q))) q++;
                    if (q < n && (s.charAt(q) == '"' || s.charAt(q) == '\'')) {
                        int e = s.indexOf(s.charAt(q), q + 1);
                        if (e < 0) e = n;
                        value = s.substring(q + 1, e);
                        q = Math.min(n, e + 1);
                    } else {
                        int v = q;
                        while (q < n && !space(s.charAt(q)) && s.charAt(q) != '>'
                                && !(s.charAt(q) == '/' && q + 1 < n && s.charAt(q + 1) == '>')) q++;
                        value = s.substring(v, q);
                    }
                    p = q;
                }
                if (p == a) p++;  // not a name character: skip it
                else el.attrs.put(name, value);
            }
        }
        add(d, open, el);
        if (!empty) open.push(el);
        return p;
    }

    private static void add(Doc d, Deque<Element> open, Node node) {
        if (open.isEmpty()) d.top.add(node);
        else open.peek().kids.add(node);
    }

    // A closing tag closes the innermost open element of that name, and any
    // left open inside it; one that matches nothing is dropped.
    private static void close(Deque<Element> open, String name) {
        for (boolean exact : new boolean[] {true, false})
            for (Element e : open)
                if (exact ? e.name.equals(name) : e.name.equalsIgnoreCase(name)) {
                    while (open.pop() != e) {
                    }
                    return;
                }
    }

    // ---------------------------------------------------------------- writing
    static String write(Doc d) {
        StringBuilder sb = new StringBuilder();
        for (Node n : d.top) write(n, sb);
        return sb.toString();
    }

    private static void write(Node n, StringBuilder sb) {
        if (n instanceof Raw) {
            sb.append(((Raw) n).raw);
            return;
        }
        Element e = (Element) n;
        sb.append('<').append(e.name);
        for (Map.Entry<String, String> a : e.attrs.entrySet()) {
            char q = a.getValue().indexOf('"') >= 0 ? '\'' : '"';
            sb.append(' ').append(a.getKey()).append('=').append(q).append(a.getValue()).append(q);
        }
        if (e.kids.isEmpty()) {
            sb.append(" />");
            return;
        }
        sb.append('>');
        for (Node k : e.kids) write(k, sb);
        sb.append("</").append(e.name).append('>');
    }

    static String decode(String s) {
        if (s.indexOf('&') < 0) return s;
        StringBuilder sb = new StringBuilder(s.length());
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            int semi = c == '&' ? s.indexOf(';', i) : -1;
            if (semi < 0 || semi - i > 10) {
                sb.append(c);
                continue;
            }
            String ent = s.substring(i + 1, semi);
            String rep = null;
            switch (ent) {
                case "lt": rep = "<"; break;
                case "gt": rep = ">"; break;
                case "amp": rep = "&"; break;
                case "quot": rep = "\""; break;
                case "apos": rep = "'"; break;
                default:
                    try {
                        if (ent.startsWith("#x")) rep = new String(Character.toChars(Integer.parseInt(ent.substring(2), 16)));
                        else if (ent.startsWith("#")) rep = new String(Character.toChars(Integer.parseInt(ent.substring(1))));
                    } catch (IllegalArgumentException ignored) {
                    }
            }
            if (rep == null) {
                sb.append(c);
            } else {
                sb.append(rep);
                i = semi;
            }
        }
        return sb.toString();
    }

    // ---------------------------------------------------------------- patching
    /** Where appended tags go: inside the <FTL> wrapper FTL 1.6's files have, else the top level. */
    private static Element root(Doc d) {
        Element only = null;
        for (Node n : d.top)
            if (n instanceof Element) {
                if (only != null) return new Element("", d.top);
                only = (Element) n;
            }
        return only != null && only.name.equalsIgnoreCase("FTL") ? only : new Element("", d.top);
    }

    /**
     * Applies an ".xml.append" file: plain tags are added at the end, mod:find...
     * tags change what they find. {@code notes} collects anything skipped.
     */
    static void append(Doc target, String patch, String where, List<String> notes) throws ModException {
        Element into = root(target);
        Doc p = parse(patch);
        Element from = root(p);
        // a patch that is only text and comments around one <FTL> has been unwrapped by root()
        for (Node n : new ArrayList<>(from.kids)) {
            if (!(n instanceof Element)) continue;
            Element e = (Element) n;
            if (isFind(e.name)) {
                for (Element hit : find(into, e, where)) apply(into, hit, e, where, notes);
            } else if (e.name.startsWith("mod:") || e.name.startsWith("mod-")) {
                if (!command(into, into, e, where, notes))
                    notes.add(where + ": <" + e.name + "> isn't supported here");
            } else {
                into.kids.add(e.copy());
            }
        }
    }

    private static boolean isFind(String name) {
        return name.equals("mod:findName") || name.equals("mod:findLike") || name.equals("mod:findWithChildLike")
                || name.equals("mod:findComposite");
    }

    private static int intAttr(Element e, String name, int def, String where) throws ModException {
        String v = e.attrs.get(name);
        if (v == null) return def;
        try {
            return Integer.parseInt(v.trim());
        } catch (NumberFormatException ex) {
            throw new ModException(where + ": <" + e.name + "> has a bad " + name + "=\"" + v + "\"");
        }
    }

    private static boolean boolAttr(Element e, String name) {
        return "true".equalsIgnoreCase(e.attrs.getOrDefault(name, "").trim());
    }

    private static List<Element> find(Element ctx, Element f, String where) throws ModException {
        List<Element> hits = new ArrayList<>();
        String type = f.attrs.get("type");
        Element selector = f.child("mod:selector");
        switch (f.name) {
            case "mod:findName": {
                String name = f.attrs.get("name");
                if (name == null) throw new ModException(where + ": <mod:findName> without a name");
                for (Element c : ctx.elements())
                    if ((type == null || c.name.equals(type)) && name.equals(c.attrs.get("name"))) hits.add(c);
                break;
            }
            case "mod:findLike":
                for (Element c : ctx.elements())
                    if ((type == null || c.name.equals(type)) && like(c, selector)) hits.add(c);
                break;
            case "mod:findWithChildLike": {
                String childType = f.attrs.get("child-type");
                for (Element c : ctx.elements()) {
                    if (type != null && !c.name.equals(type)) continue;
                    for (Element k : c.elements())
                        if ((childType == null || k.name.equals(childType)) && like(k, selector)) {
                            hits.add(c);
                            break;
                        }
                }
                break;
            }
            default: {  // mod:findComposite
                Element par = f.child("mod:par");
                if (par == null) throw new ModException(where + ": <mod:findComposite> without a <mod:par>");
                Set<Element> set = par(ctx, par, where);
                for (Element c : ctx.elements())
                    if (set.contains(c)) hits.add(c);
            }
        }
        if (boolAttr(f, "reverse")) Collections.reverse(hits);
        int start = intAttr(f, "start", 0, where);
        int limit = intAttr(f, "limit", f.name.equals("mod:findName") ? 1 : -1, where);
        hits = hits.subList(Math.min(Math.max(start, 0), hits.size()), hits.size());
        if (limit >= 0 && hits.size() > limit) hits = hits.subList(0, limit);
        if (hits.isEmpty() && boolAttr(f, "panic"))
            throw new ModException(where + ": <" + f.name + describe(f) + "> found nothing to change");
        return new ArrayList<>(hits);
    }

    private static String describe(Element f) {
        StringBuilder sb = new StringBuilder();
        for (Map.Entry<String, String> a : f.attrs.entrySet())
            if (a.getKey().equals("type") || a.getKey().equals("name"))
                sb.append(' ').append(a.getKey()).append("=\"").append(a.getValue()).append('"');
        return sb.toString();
    }

    private static boolean like(Element e, Element selector) {
        if (selector == null) return true;
        for (Map.Entry<String, String> a : selector.attrs.entrySet()) {
            String v = e.attrs.get(a.getKey());
            if (v == null || !decode(v).equals(decode(a.getValue()))) return false;
        }
        String want = selector.text().trim();
        return want.isEmpty() || e.text().trim().equals(want);
    }

    private static Set<Element> par(Element ctx, Element par, String where) throws ModException {
        String op = par.attrs.getOrDefault("op", "AND").trim().toUpperCase(java.util.Locale.ROOT);
        List<Set<Element>> sets = new ArrayList<>();
        for (Element k : par.elements()) {
            if (k.name.equals("mod:par")) sets.add(par(ctx, k, where));
            else if (isFind(k.name)) sets.add(identitySet(find(ctx, k, where)));
        }
        Set<Element> all = identitySet(ctx.elements()), out = identitySet(Collections.emptyList());
        for (Element c : all) {
            int in = 0;
            for (Set<Element> s : sets) in += s.contains(c) ? 1 : 0;
            boolean keep;
            switch (op) {
                case "OR": keep = in > 0; break;
                case "NOR": keep = in == 0; break;
                case "NAND": keep = in < sets.size(); break;
                case "XOR": keep = in == 1; break;
                case "AND": keep = in == sets.size() && !sets.isEmpty(); break;
                default: throw new ModException(where + ": <mod:par op=\"" + op + "\"> isn't a known operation");
            }
            if (keep) out.add(c);
        }
        return out;
    }

    private static Set<Element> identitySet(List<Element> items) {
        Set<Element> s = Collections.newSetFromMap(new IdentityHashMap<>());
        s.addAll(items);
        return s;
    }

    // Runs a find tag's commands on one of the tags it found (in ctx).
    private static void apply(Element ctx, Element hit, Element f, String where, List<String> notes)
            throws ModException {
        for (Element c : f.elements()) {
            if (c.name.equals("mod:selector") || c.name.equals("mod:par")) continue;
            if (isFind(c.name)) {
                for (Element h : find(hit, c, where)) apply(hit, h, c, where, notes);
            } else if (!command(ctx, hit, c, where, notes)) {
                notes.add(where + ": <" + c.name + "> isn't supported");
            }
        }
    }

    private static boolean command(Element ctx, Element hit, Element c, String where, List<String> notes) {
        String name = c.name;
        switch (name) {
            case "mod:setAttributes":
                hit.attrs.putAll(c.attrs);
                return true;
            case "mod:removeAttributes":
                for (String k : c.attrs.keySet()) hit.attrs.remove(k);
                return true;
            case "mod:setValue": {  // replaces everything inside
                StringBuilder sb = new StringBuilder();
                for (Node k : c.kids)
                    if (k instanceof Raw && ((Raw) k).text) sb.append(((Raw) k).raw);
                hit.kids.clear();
                hit.kids.add(new Raw(sb.toString(), true));
                return true;
            }
            case "mod:removeTag":
                ctx.kids.remove(hit);
                return true;
            default:
        }
        int colon = name.indexOf(':');
        if (colon < 0 || colon == name.length() - 1) return false;
        String kind = name.substring(0, colon), tag = name.substring(colon + 1);
        Element made = c.copyAs(tag);
        switch (kind) {
            case "mod-append":
                hit.kids.add(made);
                return true;
            case "mod-prepend":
                hit.kids.add(0, made);
                return true;
            case "mod-overwrite": {
                Element old = hit.child(tag);
                if (old != null) hit.kids.set(hit.kids.indexOf(old), made);
                else hit.kids.add(made);
                return true;
            }
            default:
                return false;
        }
    }
}
