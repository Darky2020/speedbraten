#!/usr/bin/env python3
"""Prunes a witness data dir to what witness-alpha-14 and later keep. Run it with the witness stopped.

  witnessprune.py <witness data dir>

- aim fingerprints of runs that never reached the finish (runs.runtimems is 0 or unset) are deleted; the witness
  only keeps those of finished runs, and -P rebuilds them from the replay files;
- judge output files resimhome/resim-*.json are deleted: each judged run's JSON is in runs.resim, and a run still
  to be judged is re-simulated on start;
- the database is compacted (VACUUM), which needs free disk space about the size of the database.

Replay files (runs/) are not touched.
"""
import os
import sqlite3
import sys
import time

def size(path):
    return sum(os.path.getsize(path + ext) for ext in ('', '-wal', '-shm') if os.path.exists(path + ext))

def mb(n):
    return '%.1f MB' % (n / 1048576)

def phase(name, start):
    print('%s: done in %.1f s' % (name, time.time() - start), flush=True)

def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    datadir = sys.argv[1]
    dbpath = os.path.join(datadir, 'witness.db')
    if not os.path.exists(dbpath):
        sys.exit('no witness.db in ' + datadir)
    before = size(dbpath)
    db = sqlite3.connect(dbpath)
    if db.execute("PRAGMA journal_mode").fetchone()[0] == 'wal':
        db.execute("PRAGMA wal_checkpoint(TRUNCATE)")

    t = time.time()
    total = db.execute("SELECT COUNT(*) FROM prints").fetchone()[0]
    keep = db.execute("SELECT COUNT(*) FROM prints WHERE run IN (SELECT id FROM runs WHERE IFNULL(runtimems, 0) > 0)").fetchone()[0]
    print('fingerprints: %d rows, keeping %d of finished runs, deleting %d' % (total, keep, total - keep), flush=True)
    db.execute("DELETE FROM prints WHERE run NOT IN (SELECT id FROM runs WHERE IFNULL(runtimems, 0) > 0)")
    db.commit()
    phase('fingerprints', t)

    t = time.time()
    resimhome = os.path.join(datadir, 'resimhome')
    files = [f for f in os.listdir(resimhome) if f.startswith('resim-') and f.endswith('.json')] if os.path.isdir(resimhome) else []
    freed = 0
    for i, f in enumerate(files):
        p = os.path.join(resimhome, f)
        freed += os.path.getsize(p)
        os.remove(p)
        if (i + 1) % 1000 == 0:
            print('judge files: %d/%d removed' % (i + 1, len(files)), flush=True)
    print('judge files: %d removed, %s' % (len(files), mb(freed)), flush=True)
    phase('judge files', t)

    t = time.time()
    print('compacting %s (this takes a while for a large database)' % mb(size(dbpath)), flush=True)
    db.execute("VACUUM")
    # in WAL mode the compacted pages land in witness.db-wal until a checkpoint writes them back
    db.execute("PRAGMA wal_checkpoint(TRUNCATE)")
    db.close()
    phase('compacting', t)
    print('witness.db: %s -> %s' % (mb(before), mb(size(dbpath))), flush=True)

if __name__ == '__main__':
    main()
