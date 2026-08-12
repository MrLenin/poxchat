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

Safety properties:
  - .corrupt.* backups are opened strictly read-only (this tool can never
    mutate an irreplaceable source, and a typo'd path raises instead of
    silently creating an empty DB).
  - The live DB is snapshotted (including any hot -journal sidecar) BEFORE
    it is ever opened read-write, so --apply always has a truly
    pre-recovery .pre-salvage copy.  Dry runs never open the real live file
    read-write at all -- they operate on a throwaway copy.
  - A reconstructed image whose schema predates the channel_id/is_user_msg
    ALTERs (see src/common/scrollback.c init_database) is migrated to the
    current schema before merging, so a live DB seeded from an old backup
    doesn't crash the whole run.
  - The re-compressed output is written to a temp file and atomically
    renamed into place.

Usage:
    python tools/scrollback-salvage.py <scrollback-dir>            # dry run
    python tools/scrollback-salvage.py <scrollback-dir> --apply

Run only with the app closed.  --apply keeps a <db>.pre-salvage.<ts> copy.
Requires: pip install zstandard
"""
import argparse
import glob
import os
import pathlib
import shutil
import sqlite3
import struct
import sys
import tempfile
import time

import zstandard

RAW, ZSTD, ZSTD_DICT = 0, 1, 2


def read_outer(path, readonly=False):
    """Return (page_size, header_count, header_trustworthy, {pgno: page_bytes}, dict_bytes)."""
    if readonly:
        # Backups are irreplaceable sources: open strictly read-only so this
        # tool is physically unable to mutate them, and so a typo'd path
        # raises immediately instead of silently creating an empty DB.
        uri = pathlib.Path(path).resolve().as_uri() + "?mode=ro"
        db = sqlite3.connect(uri, uri=True)
    else:
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
    # Per the SQLite file format, the in-header page count (offset 28) is
    # only authoritative when the 4-byte change counter (offset 24) matches
    # the 4-byte version-valid-for number (offset 92) -- the same rule
    # SQLite itself uses to decide whether to trust it.  A mismatch means
    # the header size is stale (e.g. process killed mid-commit) and must
    # not be used to truncate pages that are actually present.
    change_counter = struct.unpack(">I", hdr[24:28])[0]
    version_valid_for = struct.unpack(">I", hdr[92:96])[0]
    header_trustworthy = change_counter == version_valid_for
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
    return page_size, header_count, header_trustworthy, pages, dict_bytes


def reconstruct(outer_path, image_path, readonly=False):
    """Decompress an outer DB into a plain inner SQLite image; integrity-check it.

    Returns (count, verdict, dict_bytes, note).  note is a human-readable
    string when pages were dropped off the tail, else None.
    """
    page_size, header_count, header_trustworthy, pages, dict_bytes = read_outer(
        outer_path, readonly=readonly)
    max_pgno = max(pages)
    if header_trustworthy and header_count:
        # The inner header count is authoritative; pages beyond it are an
        # uncommitted tail (seen in one field backup) and are dropped.
        count = min(header_count, max_pgno)
    else:
        # Stale/untrustworthy header size: don't truncate pages that are
        # actually present in the pages table.
        count = max_pgno
    note = None
    if count < max_pgno:
        note = (f"dropped {max_pgno - count} uncommitted tail pages "
                f"(header says {header_count}, pages table has {max_pgno})")
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
    return count, verdict, dict_bytes, note


def table_cols(db, table):
    # PRAGMA syntax puts the schema before the pragma name, not inside the
    # argument list, so "bk.messages" must become "PRAGMA bk.table_info(messages)".
    if "." in table:
        schema, name = table.split(".", 1)
        return [r[1] for r in db.execute(f"PRAGMA {schema}.table_info({name})")]
    return [r[1] for r in db.execute(f"PRAGMA table_info({table})")]


def has_table(db, schema, name):
    return db.execute(
        f"SELECT 1 FROM {schema}.sqlite_master WHERE type='table' AND name=?",
        (name,)).fetchone() is not None


def bk_has_table(db, name):
    return has_table(db, "bk", name)


def migrate_image(image_path):
    """Bring a reconstructed image's schema up to date with the app's
    current schema (mirrors src/common/scrollback.c init_database).

    live_img can come from an existing live DB or be seeded from any
    reconstructable backup -- and 6 of the 11 field backups predate the
    channel_id/is_user_msg ALTERs.  merge_backup()'s INSERT column lists are
    static, so without this the merge crashes on a pre-ALTER image instead
    of just that one network failing.
    """
    db = sqlite3.connect(image_path)
    try:
        with db:
            db.execute(
                "CREATE TABLE IF NOT EXISTS channels ("
                "    id INTEGER PRIMARY KEY,"
                "    name TEXT NOT NULL UNIQUE"
                ")")
            mcols = table_cols(db, "messages")
            if "channel_id" not in mcols:
                db.execute("ALTER TABLE messages ADD COLUMN channel_id "
                           "INTEGER REFERENCES channels(id)")
            if "is_user_msg" not in mcols:
                db.execute("ALTER TABLE messages ADD COLUMN is_user_msg "
                           "INTEGER NOT NULL DEFAULT 0")
            has_reactions = has_table(db, "main", "reactions")
            if has_reactions:
                rcols = table_cols(db, "reactions")
                if "channel_id" not in rcols:
                    db.execute("ALTER TABLE reactions ADD COLUMN channel_id "
                               "INTEGER REFERENCES channels(id)")
            db.execute("CREATE INDEX IF NOT EXISTS idx_channel_id_time "
                       "ON messages(channel_id, timestamp)")
            if has_reactions:
                db.execute("CREATE INDEX IF NOT EXISTS idx_reactions_channel_id "
                           "ON reactions(channel_id, target_msgid)")
            # Backfill: populate channels + channel_id for rows that predate
            # the columns just added (mirrors init_database's migration block).
            legacy = db.execute(
                "SELECT DISTINCT channel FROM messages "
                "WHERE channel_id IS NULL AND channel IS NOT NULL").fetchall()
            for (ch_name,) in legacy:
                db.execute("INSERT OR IGNORE INTO channels (name) VALUES (?)",
                           (ch_name,))
                db.execute(
                    "UPDATE messages SET channel_id = "
                    "(SELECT id FROM channels WHERE name = ?) "
                    "WHERE channel = ? AND channel_id IS NULL",
                    (ch_name, ch_name))
                if has_reactions:
                    db.execute(
                        "UPDATE reactions SET channel_id = "
                        "(SELECT id FROM channels WHERE name = ?) "
                        "WHERE channel = ? AND channel_id IS NULL",
                        (ch_name, ch_name))
    finally:
        db.close()


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
    """Re-compress a plain inner image into the outer DB format.

    Builds at out_path + ".tmp" and atomically renames into place, so a
    crash or ENOSPC mid-write can never leave out_path holding a partial DB.
    """
    tmp_path = out_path + ".tmp"
    for stale in (tmp_path, tmp_path + "-journal"):
        if os.path.exists(stale):
            os.remove(stale)
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
    db = sqlite3.connect(tmp_path)
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
    os.replace(tmp_path, out_path)


def copy_with_journal(src, dst):
    """Copy src (+ its -journal sidecar, if present) to dst (+ dst-journal).
    Returns True if a journal was copied."""
    shutil.copy2(src, dst)
    journal = src + "-journal"
    has_journal = os.path.exists(journal)
    if has_journal:
        shutil.copy2(journal, dst + "-journal")
    return has_journal


def salvage_network(live_path, backups, apply_changes, workdir):
    name = os.path.basename(live_path)
    print(f"\n=== {name}: {len(backups)} backup(s) ===")
    live_img = os.path.join(workdir, name + ".live.img")
    dict_bytes = None
    if os.path.exists(live_path):
        if apply_changes:
            # Snapshot the live file (and any hot -journal) BEFORE the very
            # first read-write open below, which may roll a hot journal
            # forward and delete it -- this is the only chance to keep a
            # truly pre-recovery copy.
            keep = f"{live_path}.pre-salvage.{int(time.time())}"
            had_journal = copy_with_journal(live_path, keep)
            print(f"  saved {os.path.basename(keep)}"
                  + (" (+ journal)" if had_journal else ""))
            read_target = live_path
        else:
            # Dry run must never mutate the real live file -- reconstruct()
            # opens read-write specifically so it can roll a hot journal
            # forward, so operate on a throwaway copy instead.
            read_target = os.path.join(workdir, name + ".live-probe")
            copy_with_journal(live_path, read_target)
        count, verdict, dict_bytes, note = reconstruct(read_target, live_img)
        if note:
            print(f"  {note}")
        print(f"  live: {count} pages, integrity: {verdict}")
        if verdict != "ok":
            print(f"  !! live DB failed integrity ({verdict}) — skipping network")
            return
        migrate_image(live_img)
    else:
        # No live DB: seed from the newest reconstructable backup.  Backups
        # are opened read-only -- they are irreplaceable and must never be
        # mutated by this tool.
        for bk in sorted(backups, reverse=True):
            try:
                count, verdict, dict_bytes, note = reconstruct(bk, live_img, readonly=True)
            except (ValueError, sqlite3.Error) as e:
                print(f"  !! seed {os.path.basename(bk)}: {e}")
                continue
            if verdict == "ok":
                if note:
                    print(f"  {note}")
                print(f"  seeded from {os.path.basename(bk)} ({count} pages)")
                backups = [b for b in backups if b != bk]
                break
        else:
            print("  !! no usable backup to seed from — skipping network")
            return
        migrate_image(live_img)

    for bk in sorted(backups):  # oldest first
        bk_img = os.path.join(workdir, os.path.basename(bk) + ".img")
        try:
            count, verdict, _, note = reconstruct(bk, bk_img, readonly=True)
        except (ValueError, sqlite3.Error) as e:
            print(f"  !! {os.path.basename(bk)}: reconstruction failed: {e}")
            continue
        if note:
            print(f"  {os.path.basename(bk)}: {note}")
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
        write_outer(live_img, live_path, dict_bytes)
        # Any -journal here belonged to the pre-salvage generation of this
        # file (already preserved above) and is stale w.r.t. the content
        # just written -- remove it so nothing is left for a future opener
        # to puzzle over.
        stale_journal = live_path + "-journal"
        if os.path.exists(stale_journal):
            os.remove(stale_journal)
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
