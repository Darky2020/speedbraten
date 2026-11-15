package hub

import (
	"bufio"
	"os"
	"strconv"
	"strings"
)

// Server is a game server online life may be played on.
type Server struct {
	Host  string
	Port  int
	Label string
}

// readServers reads a servers file: one server per line, "host port label" (the label may hold
// spaces), blank lines and lines starting with # skipped. Malformed lines are skipped and reported.
func readServers(path string, logf func(string, ...any)) []Server {
	f, err := os.Open(path)
	if err != nil {
		logf("servers: %v", err)
		return nil
	}
	defer f.Close()
	var out []Server
	sc := bufio.NewScanner(f)
	for n := 1; sc.Scan(); n++ {
		line := strings.TrimSpace(sc.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		fields := strings.Fields(line)
		port, err := strconv.Atoi(func() string {
			if len(fields) > 1 {
				return fields[1]
			}
			return ""
		}())
		if len(fields) < 3 || err != nil || port <= 0 || port > 65535 || len(fields[0]) > 255 {
			logf("servers: %s line %d: want \"host port label\"", path, n)
			continue
		}
		label := strings.Join(fields[2:], " ")
		if len(label) > 100 {
			label = label[:100]
		}
		out = append(out, Server{fields[0], port, label})
	}
	return out
}

// serversMsg lists the servers file's servers, read afresh so that edits apply to the next joins.
func (h *Hub) serversMsg() []byte {
	var servers []Server
	if h.cfg.Servers != "" {
		servers = readServers(h.cfg.Servers, h.cfg.Logf)
	}
	var w writer
	w.u(uint64(len(servers)))
	for _, s := range servers {
		w.str(s.Host)
		w.u(uint64(s.Port))
		w.str(s.Label)
	}
	return w.b
}

// serverStatus takes the game server a player says it is on (SERVER: host, port, its client number
// there; an empty host for none). Only listed servers count. Its spectators here lose the view it no
// longer streams.
func (h *Hub) serverStatus(p *player, r *reader) error {
	host, port, cn := r.str(255), r.u(), r.s()
	if err := r.done(); err != nil {
		return err
	}
	var on *Server
	if host != "" && h.cfg.Servers != "" {
		for _, s := range readServers(h.cfg.Servers, h.cfg.Logf) {
			if s.Host == host && uint64(s.Port) == port {
				on = &s
				break
			}
		}
	}
	if on == nil {
		cn = 0
	}
	if (on == nil) == (p.server == nil) && (on == nil || *on == *p.server) && cn == p.servercn {
		return nil
	}
	p.server, p.servercn = on, cn
	if on != nil {
		h.endSpectators(p, p.name+" joined "+on.Label)
	}
	if p.name != "" {
		h.broadcast(MsgPlayer, h.playerMsg(p))
	}
	return nil
}
