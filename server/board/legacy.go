package board

import (
	"errors"
	"fmt"
	"strconv"
	"strings"
)

// Legacy is a board's reference record from an older leaderboard: not witnessed, not ranked.
type Legacy struct {
	Key    Key
	Player string
	MS     int64
	Source string // e.g. flagruns.com
	Date   string // as the source gives it, may be empty
	Link   string // may be empty
}

// LegacySources are the sources the admin page offers; others can be typed in the CSV.
var LegacySources = []string{"flagruns.com", "speedyruns", "speedrun.com"}

func (s *Store) legacyTable() error {
	_, err := s.site.Exec("CREATE TABLE IF NOT EXISTS legacy(map TEXT NOT NULL, kind TEXT NOT NULL, mode INTEGER NOT NULL, player TEXT NOT NULL, ms INTEGER NOT NULL, source TEXT NOT NULL, date TEXT, link TEXT, PRIMARY KEY(map, kind, mode))")
	return err
}

const legacyColumns = "map, kind, mode, player, ms, source, IFNULL(date, ''), IFNULL(link, '')"

func legacyOf(r []string) Legacy {
	mode, _ := strconv.Atoi(r[2])
	return Legacy{Key: Key{Map: r[0], Kind: r[1], Mode: mode}, Player: r[3], MS: atoi(r[4]), Source: r[5], Date: r[6], Link: r[7]}
}

// LegacyRecord is the board's legacy record, nil when it has none.
func (s *Store) LegacyRecord(k Key) (*Legacy, error) {
	rows, err := s.site.Query("SELECT "+legacyColumns+" FROM legacy WHERE map = ? AND kind = ? AND mode = ?", k.Map, k.Kind, k.Mode)
	if err != nil || len(rows) == 0 {
		return nil, err
	}
	l := legacyOf(rows[0])
	return &l, nil
}

// LegacyRecords lists every legacy record by map, kind and mode.
func (s *Store) LegacyRecords() ([]Legacy, error) {
	rows, err := s.site.Query("SELECT " + legacyColumns + " FROM legacy ORDER BY map, kind, mode")
	var out []Legacy
	for _, r := range rows {
		out = append(out, legacyOf(r))
	}
	return out, err
}

// SetLegacy adds a board's legacy record or replaces the one it has.
func (s *Store) SetLegacy(l Legacy) error {
	if err := l.check(); err != nil {
		return err
	}
	_, err := s.site.Exec("INSERT OR REPLACE INTO legacy(map, kind, mode, player, ms, source, date, link) VALUES(?, ?, ?, ?, ?, ?, NULLIF(?, ''), NULLIF(?, ''))",
		l.Key.Map, l.Key.Kind, l.Key.Mode, l.Player, l.MS, l.Source, l.Date, l.Link)
	return err
}

func (s *Store) DeleteLegacy(k Key) error {
	_, err := s.site.Exec("DELETE FROM legacy WHERE map = ? AND kind = ? AND mode = ?", k.Map, k.Kind, k.Mode)
	return err
}

func (l Legacy) check() error {
	switch {
	case l.Key.Map == "" || l.Player == "" || l.Source == "":
		return errors.New("map, player and source are needed")
	case l.Key.Kind != "flagrun" && l.Key.Kind != "race" && l.Key.Kind != "campaign" && l.Key.Kind != "dmsp":
		return fmt.Errorf("unknown kind %q", l.Key.Kind)
	case l.MS <= 0:
		return errors.New("the time is needed")
	case l.Link != "" && !strings.HasPrefix(l.Link, "https://") && !strings.HasPrefix(l.Link, "http://"):
		return errors.New("the link must start with http:// or https://")
	}
	return nil
}

// ParseTime reads a run time as m:ss.mmm, ss.mmm or h:mm:ss.mmm (fewer decimals allowed).
func ParseTime(s string) (int64, error) {
	s = strings.TrimSpace(s)
	var ms int64
	whole := s
	if i := strings.IndexByte(s, '.'); i >= 0 {
		whole = s[:i]
		frac := s[i+1:]
		if frac == "" || len(frac) > 3 {
			return 0, fmt.Errorf("bad time %q", s)
		}
		n, err := strconv.ParseInt(frac+strings.Repeat("0", 3-len(frac)), 10, 64)
		if err != nil {
			return 0, fmt.Errorf("bad time %q", s)
		}
		ms = n
	}
	var secs int64
	for _, part := range strings.Split(whole, ":") {
		n, err := strconv.ParseInt(part, 10, 64)
		if err != nil || n < 0 {
			return 0, fmt.Errorf("bad time %q", s)
		}
		secs = secs*60 + n
	}
	return secs*1000 + ms, nil
}

// ParseLegacyCSV reads lines of map,kind,mode,player,time,source[,date[,link]]; mode is the
// game's number or name (effic ctf). Blank lines and lines starting with # are skipped.
func ParseLegacyCSV(text string) ([]Legacy, error) {
	var out []Legacy
	for n, line := range strings.Split(text, "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		f := strings.Split(line, ",")
		for i := range f {
			f[i] = strings.TrimSpace(f[i])
		}
		if len(f) < 6 || len(f) > 8 {
			return nil, fmt.Errorf("line %d: want map,kind,mode,player,time,source[,date[,link]]", n+1)
		}
		for len(f) < 8 {
			f = append(f, "")
		}
		mode, ok := ParseMode(f[2])
		if !ok {
			return nil, fmt.Errorf("line %d: unknown mode %q", n+1, f[2])
		}
		ms, err := ParseTime(f[4])
		if err != nil {
			return nil, fmt.Errorf("line %d: %v", n+1, err)
		}
		l := Legacy{Key: Key{Map: f[0], Kind: f[1], Mode: mode}, Player: f[3], MS: ms, Source: f[5], Date: f[6], Link: f[7]}
		if err := l.check(); err != nil {
			return nil, fmt.Errorf("line %d: %v", n+1, err)
		}
		out = append(out, l)
	}
	return out, nil
}

// ParseMode reads a mode as the game's number or its name.
func ParseMode(s string) (int, bool) {
	if n, err := strconv.Atoi(s); err == nil {
		return n, true
	}
	for i, name := range modeNames {
		if strings.EqualFold(name, s) {
			return i - 3, true
		}
	}
	return 0, false
}
