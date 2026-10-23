package hub

import (
	"errors"
	"strconv"
	"strings"
	"time"

	"speedbraten/server/sqlite"
)

const schema = `
CREATE TABLE IF NOT EXISTS installs (
	key TEXT PRIMARY KEY,
	name TEXT,
	namekey TEXT,
	firstaddr TEXT NOT NULL,
	firstseen INTEGER NOT NULL,
	lastseen INTEGER NOT NULL,
	muteduntil INTEGER NOT NULL DEFAULT 0
);
CREATE UNIQUE INDEX IF NOT EXISTS installs_namekey ON installs(namekey);
CREATE INDEX IF NOT EXISTS installs_firstaddr ON installs(firstaddr, firstseen);
CREATE TABLE IF NOT EXISTS chat (
	time INTEGER NOT NULL,
	key TEXT NOT NULL,
	name TEXT NOT NULL,
	scope INTEGER NOT NULL,
	lobby INTEGER NOT NULL,
	text TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS chat_time ON chat(time);
CREATE TABLE IF NOT EXISTS reports (
	time INTEGER NOT NULL,
	reporter TEXT NOT NULL,
	reported TEXT NOT NULL,
	name TEXT NOT NULL,
	reason TEXT NOT NULL,
	reviewed INTEGER NOT NULL DEFAULT 0
);
`

type store struct{ db *sqlite.DB }

func openStore(path string) (*store, error) {
	db, err := sqlite.Create(path)
	if err != nil {
		return nil, err
	}
	for _, q := range append([]string{"PRAGMA journal_mode=WAL"}, strings.Split(schema, ";")...) {
		if strings.TrimSpace(q) == "" {
			continue
		}
		if _, err := db.Query(q); err != nil {
			db.Close()
			return nil, err
		}
	}
	return &store{db}, nil
}

type install struct {
	known      bool
	name       string
	mutedUntil int64
}

func (s *store) install(key string) (install, error) {
	rows, err := s.db.Query("SELECT IFNULL(name, ''), muteduntil FROM installs WHERE key = ?", key)
	if err != nil || len(rows) == 0 {
		return install{}, err
	}
	muted, _ := strconv.ParseInt(rows[0][1], 10, 64)
	return install{known: true, name: rows[0][0], mutedUntil: muted}, nil
}

func (s *store) newInstallsSince(addr string, since int64) (int, error) {
	rows, err := s.db.Query("SELECT COUNT(*) FROM installs WHERE firstaddr = ? AND firstseen >= ?", addr, since)
	if err != nil {
		return 0, err
	}
	n, _ := strconv.Atoi(rows[0][0])
	return n, nil
}

func (s *store) addInstall(key, addr string, now int64) error {
	_, err := s.db.Exec("INSERT INTO installs (key, firstaddr, firstseen, lastseen) VALUES (?, ?, ?, ?)", key, addr, now, now)
	return err
}

func (s *store) seen(key string, now int64) error {
	_, err := s.db.Exec("UPDATE installs SET lastseen = ? WHERE key = ?", now, key)
	return err
}

var errNameTaken = errors.New("taken")

// nameKey is what makes two names the same name: case is ignored, in any script.
func nameKey(name string) string { return strings.ToLower(name) }

// setName gives key the name, freeing its old one; errNameTaken if another install has it.
func (s *store) setName(key, name string) error {
	rows, err := s.db.Query("SELECT key FROM installs WHERE namekey = ?", nameKey(name))
	if err != nil {
		return err
	}
	if len(rows) > 0 && rows[0][0] != key {
		return errNameTaken
	}
	_, err = s.db.Exec("UPDATE installs SET name = ?, namekey = ? WHERE key = ?", name, nameKey(name), key)
	return err
}

func (s *store) keyForName(name string) (string, error) {
	rows, err := s.db.Query("SELECT key FROM installs WHERE namekey = ?", nameKey(name))
	if err != nil || len(rows) == 0 {
		return "", err
	}
	return rows[0][0], nil
}

func (s *store) mute(key string, until int64) error {
	_, err := s.db.Exec("UPDATE installs SET muteduntil = ? WHERE key = ?", until, key)
	return err
}

func (s *store) logChat(now int64, key, name string, scope int, lobby uint64, text string) error {
	_, err := s.db.Exec("INSERT INTO chat (time, key, name, scope, lobby, text) VALUES (?, ?, ?, ?, ?, ?)", now, key, name, scope, int64(lobby), text)
	return err
}

func (s *store) report(now int64, reporter, reported, name, reason string) error {
	_, err := s.db.Exec("INSERT INTO reports (time, reporter, reported, name, reason) VALUES (?, ?, ?, ?, ?)", now, reporter, reported, name, reason)
	return err
}

func (s *store) pruneChat(before int64) (int, error) {
	return s.db.Exec("DELETE FROM chat WHERE time < ?", before)
}

const chatKeep = 30 * 24 * time.Hour
