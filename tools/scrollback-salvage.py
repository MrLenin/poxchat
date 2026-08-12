#!/usr/bin/env python3
"""Salvage poxchat compressed scrollback databases.

The on-disk scrollback file is a plain SQLite DB (the "outer" DB) holding
zstd-compressed 4096-byte pages of an "inner" SQLite DB:
    pages(pgno INTEGER PRIMARY KEY, data BLOB, is_compressed INTEGER)
    meta(key TEXT PRIMARY KEY, value BLOB)   -- page_size, page_count, zstd_dict
is_compressed: 0 = raw, 1 = zstd, 2 = zstd + dictionary.  Page 1 is raw.

Pre-fix builds only persisted meta.page_count at clean close, so any process
kill left it stale; the app then declared the DB corrupt and renamed it to
<db>.corrupt.<timestamp>.  The page data in those backups is intact.  This
tool reconstructs each backup, merges its rows into the live DB (dedup by
msgid where present, by channel/timestamp/text otherwise), and re-compresses.

Usage:
    python tools/scrollback-salvage.py <scrollback-dir>            # dry run
    python tools/scrollback-salvage.py <scrollback-dir> --apply

Run only with the app closed.  --apply keeps a <db>.pre-salvage.<ts> copy.
Requires: pip install zstandard
"""
import argparse
import glob
import os
import shutil
import sqlite3
import struct
import sys
import tempfile
import time

import zstandard

RAW, ZSTD, ZSTD_DICT = 0, 1, 2


def read_outer(path):
    """Return (page_size, header_count, {pgno: page_bytes}, dict_bytes)."""
    db = sqlite3.connect(path)  # rw open so a leftover -journal is recovered
    try:
        meta = dict(db.execute("SELECT key, value FROM meta").fetchall())
        dict_bytes = meta.get("zstd_dict")
        dctx_plain = zstandard.ZstdDecompressor()
        dctx_dict = (zstandard.ZstdDecompressor(
                        dict_data=zstandard.ZstdCompressionDict(dict_bytes))
                     if dict_bytes else None)
        rows = db.execute(
            "SELECT pgno, data, is_compressed FROM pages ORDER BY pgno").fetchall()
    finally:
        db.close()
    if not rows or rows[0][0] != 1 or rows[0][2] != RAW:
        raise ValueError("page 1 missing or not stored raw")
    hdr = rows[0][1]
    ps = (hdr[16] << 8) | hdr[17]
    page_size = 65536 if ps == 1 else ps
    header_count = struct.unpack(">I", hdr[28:32])[0]
    pages = {}
    for pgno, data, method in rows:
        if method == RAW:
            img = bytes(data)
            if len(img) < page_size:
                img += b"\0" * (page_size - len(img))
        elif method == ZSTD:
            img = dctx_plain.decompress(data, max_output_size=page_size)
        elif method == ZSTD_DICT:
            if not dctx_dict:
                raise ValueError(f"page {pgno}: dict-compressed but no dict stored")
            img = dctx_dict.decompress(data, max_output_size=page_size)
        else:
            raise ValueError(f"page {pgno}: unknown method {method}")
        if len(img) != page_size:
            raise ValueError(f"page {pgno}: {len(img)} bytes, expected {page_size}")
        pages[pgno] = img
    return page_size, header_count, pages, dict_bytes


def reconstruct(outer_path, image_path):
    """Decompress an outer DB into a plain inner SQLite image; integrity-check it."""
    page_size, header_count, pages, dict_bytes = read_outer(outer_path)
    max_pgno = max(pages)
    # The inner header count is authoritative; pages beyond it are an
    # uncommitted tail (seen in one field backup) and are dropped.
    count = min(header_count, max_pgno) if header_count else max_pgno
    missing = [p for p in range(1, count + 1) if p not in pages]
    if missing:
        raise ValueError(f"missing pages within header count: {missing[:10]}")
    with open(image_path, "wb") as fh:
        for p in range(1, count + 1):
            fh.write(pages[p])
    db = sqlite3.connect(image_path)
    try:
        verdict = db.execute("PRAGMA integrity_check").fetchone()[0]
    finally:
        db.close()
    return count, verdict, dict_bytes


def table_cols(db, table):
    # PRAGMA syntax puts the schema before the pragma name, not inside the
    # argument list, so "bk.messages" must become "PRAGMA bk.table_info(messages)".
    if "." in table:
        schema, name = table.split(".", 1)
        return [r[1] for r in db.execute(f"PRAGMA {schema}.table_info({name})")]
    return [r[1] for r in db.execute(f"PRAGMA table_info({table})")]


def bk_has_table(db, name):
    return db.execute(
        "SELECT 1 FROM bk.sqlite_master WHERE type='table' AND name=?",
        (name,)).fetchone() is not None


def merge_backup(live_image, backup_image):
    """Merge rows from backup_image into live_image.  Returns stats dict."""
    db = sqlite3.connect(live_image)
    stats = {}
    try:
        db.execute("ATTACH ? AS bk", (backup_image,))
        bcols = table_cols(db, "bk.messages")
        # Channel name of a backup row (channel_id may not exist / be NULL)
        if "channel_id" in bcols and bk_has_table(db, "channels"):
            ch = ("COALESCE((SELECT name FROM bk.channels c WHERE c.id = b.channel_id),"
                  " b.channel)")
        else:
            ch = "b.channel"
        ium = "b.is_user_msg" if "is_user_msg" in bcols else "0"

        with db:  # one transaction for the whole merge
            db.execute(f"INSERT OR IGNORE INTO channels (name) "
                       f"SELECT DISTINCT {ch} FROM bk.messages b")

            before = db.execute("SELECT COUNT(*) FROM messages").fetchone()[0]
            # msgid rows: idx_msgid (partial UNIQUE) dedupes via OR IGNORE
            db.execute(f"""
                INSERT OR IGNORE INTO messages
                    (channel, timestamp, msgid, text,
                     redacted_by, redact_reason, redact_time,
                     channel_id, is_user_msg)
                SELECT {ch}, b.timestamp, b.msgid, b.text,
                       b.redacted_by, b.redact_reason, b.redact_time,
                       (SELECT id FROM channels ch2 WHERE ch2.name = {ch}), {ium}
                FROM bk.messages b WHERE b.msgid IS NOT NULL""")
            # msgid-less rows (events): dedupe on channel/timestamp/text
            db.execute(f"""
                INSERT INTO messages
                    (channel, timestamp, msgid, text,
                     redacted_by, redact_reason, redact_time,
                     channel_id, is_user_msg)
                SELECT {ch}, b.timestamp, NULL, b.text,
                       b.redacted_by, b.redact_reason, b.redact_time,
                       (SELECT id FROM channels ch2 WHERE ch2.name = {ch}), {ium}
                FROM bk.messages b
                WHERE b.msgid IS NULL AND NOT EXISTS
                      (SELECT 1 FROM messages m
                       WHERE m.msgid IS NULL AND m.timestamp = b.timestamp
                         AND m.text = b.text AND m.channel = {ch})""")
            stats["messages"] = (
                db.execute("SELECT COUNT(*) FROM messages").fetchone()[0] - before)

            if bk_has_table(db, "reactions"):
                before = db.execute("SELECT COUNT(*) FROM reactions").fetchone()[0]
                rcols = table_cols(db, "bk.reactions")
                if "channel_id" in rcols and bk_has_table(db, "channels"):
                    rch = ("COALESCE((SELECT name FROM bk.channels c "
                           "WHERE c.id = b.channel_id), b.channel)")
                else:
                    rch = "b.channel"
                db.execute(f"""
                    INSERT OR IGNORE INTO reactions
                        (channel, target_msgid, reaction_text, nick,
                         is_self, timestamp, channel_id)
                    SELECT {rch}, b.target_msgid, b.reaction_text, b.nick,
                           b.is_self, b.timestamp,
                           (SELECT id FROM channels ch2 WHERE ch2.name = {rch})
                    FROM bk.reactions b""")
                stats["reactions"] = (
                    db.execute("SELECT COUNT(*) FROM reactions").fetchone()[0] - before)

            if bk_has_table(db, "replies"):
                before = db.execute("SELECT COUNT(*) FROM replies").fetchone()[0]
                db.execute("""
                    INSERT OR IGNORE INTO replies
                        (msgid, target_msgid, target_nick, target_preview)
                    SELECT b.msgid, b.target_msgid, b.target_nick, b.target_preview
                    FROM bk.replies b""")
                stats["replies"] = (
                    db.execute("SELECT COUNT(*) FROM replies").fetchone()[0] - before)
        db.execute("DETACH bk")
    finally:
        db.close()
    return stats


def write_outer(image_path, out_path, dict_bytes):
    """Re-compress a plain inner image into the outer DB format."""
    if os.path.exists(out_path):
        os.remove(out_path)
    with open(image_path, "rb") as fh:
        data = fh.read()
    hdr = data[:100]
    ps = (hdr[16] << 8) | hdr[17]
    page_size = 65536 if ps == 1 else ps
    if len(data) % page_size:
        raise ValueError(f"image size {len(data)} not a multiple of {page_size}")
    n = len(data) // page_size
    cd = zstandard.ZstdCompressionDict(dict_bytes) if dict_bytes else None
    cctx_dict = zstandard.ZstdCompressor(level=3, dict_data=cd) if cd else None
    cctx = zstandard.ZstdCompressor(level=3)
    db = sqlite3.connect(out_path)
    try:
        db.executescript(
            "CREATE TABLE pages (pgno INTEGER PRIMARY KEY, data BLOB NOT NULL,"
            " is_compressed INTEGER NOT NULL DEFAULT 1);"
            "CREATE TABLE meta (key TEXT PRIMARY KEY, value BLOB);")
        with db:
            for i in range(n):
                pgno = i + 1
                pg = data[i * page_size:(i + 1) * page_size]
                if pgno == 1:
                    db.execute("INSERT INTO pages VALUES (?,?,?)",
                               (pgno, pg, RAW))
                    continue
                if cctx_dict:
                    comp, method = cctx_dict.compress(pg), ZSTD_DICT
                else:
                    comp, method = cctx.compress(pg), ZSTD
                if len(comp) >= page_size:
                    comp, method = pg, RAW
                db.execute("INSERT INTO pages VALUES (?,?,?)",
                           (pgno, comp, method))
            db.execute("INSERT INTO meta VALUES ('page_size', ?)", (str(page_size),))
            db.execute("INSERT INTO meta VALUES ('page_count', ?)", (str(n),))
            if dict_bytes:
                db.execute("INSERT INTO meta VALUES ('zstd_dict', ?)",
                           (sqlite3.Binary(dict_bytes),))
    finally:
        db.close()


def salvage_network(live_path, backups, apply_changes, workdir):
    name = os.path.basename(live_path)
    print(f"\n=== {name}: {len(backups)} backup(s) ===")
    live_img = os.path.join(workdir, name + ".live.img")
    dict_bytes = None
    if os.path.exists(live_path):
        count, verdict, dict_bytes = reconstruct(live_path, live_img)
        print(f"  live: {count} pages, integrity: {verdict}")
        if verdict != "ok":
            print(f"  !! live DB failed integrity ({verdict}) — skipping network")
            return
    else:
        # No live DB: seed from the newest reconstructable backup
        for bk in sorted(backups, reverse=True):
            try:
                count, verdict, dict_bytes = reconstruct(bk, live_img)
            except ValueError as e:
                print(f"  !! seed {os.path.basename(bk)}: {e}")
                continue
            if verdict == "ok":
                print(f"  seeded from {os.path.basename(bk)} ({count} pages)")
                backups = [b for b in backups if b != bk]
                break
        else:
            print("  !! no usable backup to seed from — skipping network")
            return

    for bk in sorted(backups):  # oldest first
        bk_img = os.path.join(workdir, os.path.basename(bk) + ".img")
        try:
            count, verdict, _ = reconstruct(bk, bk_img)
        except ValueError as e:
            print(f"  !! {os.path.basename(bk)}: reconstruction failed: {e}")
            continue
        if verdict != "ok":
            print(f"  !! {os.path.basename(bk)}: integrity: {verdict} — skipped")
            continue
        stats = merge_backup(live_img, bk_img)
        print(f"  merged {os.path.basename(bk)}: +{stats}")

    db = sqlite3.connect(live_img)
    try:
        verdict = db.execute("PRAGMA integrity_check").fetchone()[0]
        total = db.execute("SELECT COUNT(*) FROM messages").fetchone()[0]
    finally:
        db.close()
    print(f"  merged result: {total} messages, integrity: {verdict}")
    if verdict != "ok":
        print("  !! merged image failed integrity — NOT applying")
        return

    if apply_changes:
        if os.path.exists(live_path):
            keep = f"{live_path}.pre-salvage.{int(time.time())}"
            shutil.copy2(live_path, keep)
            print(f"  saved {os.path.basename(keep)}")
        write_outer(live_img, live_path, dict_bytes)
        print(f"  wrote {name}")
    else:
        print("  (dry run — use --apply to write)")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scrollback_dir")
    ap.add_argument("--apply", action="store_true",
                    help="write merged DBs (default: dry run)")
    args = ap.parse_args()

    by_network = {}
    for bk in glob.glob(os.path.join(args.scrollback_dir, "*.corrupt.*")):
        base = bk[:bk.index(".corrupt.")]
        by_network.setdefault(base, []).append(bk)
    if not by_network:
        print("no .corrupt backups found")
        return 0

    with tempfile.TemporaryDirectory() as workdir:
        for live_path in sorted(by_network):
            salvage_network(live_path, by_network[live_path], args.apply, workdir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
