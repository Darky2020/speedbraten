// Package hub is the speedbraten hub: players connect over noisechan, register a name for
// their install key, see who is online, gather in lobbies and chat. Lobbies and presence live
// in memory; names, the chat log and reports are kept in SQLite.
package hub

import (
	"encoding/hex"
	"errors"
	"fmt"
	"log"
	"net"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode"
	"unicode/utf8"

	"github.com/flynn/noise"

	"speedbraten/server/noisechan"
)

// Limits; a variable so tests can lower them.
var (
	MaxConnsPerAddr      = 4
	MaxNewInstallsPerDay = 5
	MaxLobbies           = 1000
	MaxLobbyMembers      = 32
	MaxChat              = 200
	OutQueue             = 512
	HandshakeTimeout     = 10 * time.Second
	MsgRate, MsgBurst    = 30.0, 60.0
	TalkRate, TalkBurst  = 0.5, 5.0
	PosRate, PosBurst    = 25.0, 50.0
)

type Config struct {
	Key    noise.DHKey
	DB     string
	Admins map[string]bool // install keys, hex
	Now    func() time.Time
	Logf   func(format string, args ...any)
}

type Hub struct {
	cfg      Config
	st       *store
	keyReply []byte

	mu        sync.Mutex
	nextID    uint64
	nextLobby uint64
	players   map[uint64]*player
	byKey     map[string]*player
	byAddr    map[string]int
	lobbies   map[uint64]*lobby
	witness   []byte
}

type player struct {
	id         uint64
	key, addr  string
	name       string
	state      int
	mapname    string
	mode       int64
	kind       string
	lobby      *lobby
	admin      bool
	mutedUntil int64
	msgs, talk bucket
	pos        bucket
	out        chan []byte
	closed     bool
}

type lobby struct {
	id      uint64
	host    *player
	mapname string
	mode    int64
	kind    string
	members []*player
}

type bucket struct {
	tokens float64
	last   time.Time
}

func (b *bucket) take(now time.Time, rate, burst float64) bool {
	if b.last.IsZero() {
		b.tokens = burst
	} else {
		b.tokens = min(burst, b.tokens+now.Sub(b.last).Seconds()*rate)
	}
	b.last = now
	if b.tokens < 1 {
		return false
	}
	b.tokens--
	return true
}

func New(cfg Config) (*Hub, error) {
	if cfg.Now == nil {
		cfg.Now = time.Now
	}
	if cfg.Logf == nil {
		cfg.Logf = log.Printf
	}
	st, err := openStore(cfg.DB)
	if err != nil {
		return nil, err
	}
	reply := append([]byte("SBRH\x01"), cfg.Key.Public...)
	return &Hub{cfg: cfg, st: st, keyReply: reply, players: map[uint64]*player{}, byKey: map[string]*player{}, byAddr: map[string]int{}, lobbies: map[uint64]*lobby{}}, nil
}

func (h *Hub) Close() { h.st.db.Close() }

// Serve runs one client connection until it ends; addr is the client's address without port.
func (h *Hub) Serve(c net.Conn, addr string) {
	defer c.Close()
	h.mu.Lock()
	if h.byAddr[addr] >= MaxConnsPerAddr {
		h.mu.Unlock()
		return
	}
	h.byAddr[addr]++
	h.mu.Unlock()
	defer func() {
		h.mu.Lock()
		if h.byAddr[addr]--; h.byAddr[addr] <= 0 {
			delete(h.byAddr, addr)
		}
		h.mu.Unlock()
	}()

	c.SetDeadline(h.cfg.Now().Add(HandshakeTimeout))
	ch, payload, err := noisechan.Accept(c, h.cfg.Key, []byte(Prologue), []byte{Proto}, h.keyReply)
	if errors.Is(err, noisechan.ErrKeyRequest) {
		return
	}
	if err != nil {
		h.cfg.Logf("%s: %v", addr, err)
		return
	}
	build := string(payload[:min(len(payload), 64)])
	typ, msg, err := ch.Read()
	if err != nil || typ != MsgHello {
		h.cfg.Logf("%s: no hello", addr)
		return
	}
	r := reader{b: msg}
	want := r.str(64)
	if r.done() != nil {
		return
	}
	c.SetDeadline(time.Time{})

	key := hex.EncodeToString(ch.PeerStatic())
	p, refusal := h.register(key, addr)
	if p == nil {
		var w writer
		w.str(refusal)
		ch.Write(MsgNotice, w.b)
		h.cfg.Logf("%s %s: %s", addr, key[:16], refusal)
		return
	}
	h.cfg.Logf("%s %s: connected (%s)", addr, key[:16], build)

	done := make(chan struct{})
	go func() {
		defer close(done)
		for m := range p.out {
			if ch.Write(m[0], m[1:]) != nil {
				break
			}
		}
		c.Close()
		for range p.out {
		}
	}()

	h.mu.Lock()
	h.join(p, want)
	h.mu.Unlock()
	for {
		typ, msg, err := ch.Read()
		if err != nil {
			break
		}
		h.mu.Lock()
		if typ == MsgPos {
			h.relayPos(p, msg)
		} else if !p.msgs.take(h.cfg.Now(), MsgRate, MsgBurst) {
			h.kick(p, "too many messages")
		} else if err := h.handle(p, typ, msg); err != nil {
			h.cfg.Logf("%s %s: %v", addr, key[:16], err)
			p.close()
		}
		closed := p.closed
		h.mu.Unlock()
		if closed {
			break
		}
	}
	h.mu.Lock()
	h.leave(p)
	p.close()
	h.mu.Unlock()
	<-done
	h.cfg.Logf("%s %s: gone", addr, key[:16])
}

func (h *Hub) register(key, addr string) (*player, string) {
	now := h.cfg.Now()
	in, err := h.st.install(key)
	if err != nil {
		return nil, "the hub had a problem, try later"
	}
	if !in.known {
		n, err := h.st.newInstallsSince(addr, now.Add(-24*time.Hour).Unix())
		if err != nil {
			return nil, "the hub had a problem, try later"
		}
		if n >= MaxNewInstallsPerDay {
			return nil, "too many new installs from your address today"
		}
		if err := h.st.addInstall(key, addr, now.Unix()); err != nil {
			return nil, "the hub had a problem, try later"
		}
	} else {
		h.st.seen(key, now.Unix())
	}
	return &player{key: key, addr: addr, name: in.name, mutedUntil: in.mutedUntil, admin: h.cfg.Admins[key], out: make(chan []byte, OutQueue)}, ""
}

// Everything below runs with h.mu held.

func (p *player) close() {
	if !p.closed {
		p.closed = true
		close(p.out)
	}
}

func (h *Hub) send(p *player, typ byte, payload []byte) {
	if p.closed {
		return
	}
	m := append([]byte{typ}, payload...)
	select {
	case p.out <- m:
	default:
		p.close()
	}
}

func (h *Hub) notice(p *player, text string) {
	var w writer
	w.str(text)
	h.send(p, MsgNotice, w.b)
}

func (h *Hub) kick(p *player, why string) {
	h.notice(p, why)
	p.close()
}

func (h *Hub) broadcast(typ byte, payload []byte) {
	for _, q := range h.players {
		h.send(q, typ, payload)
	}
}

func (h *Hub) playerMsg(p *player) []byte {
	var w writer
	w.u(p.id)
	w.str(p.name)
	w.u(uint64(p.state))
	w.str(p.mapname)
	w.s(p.mode)
	var lid uint64
	if p.lobby != nil {
		lid = p.lobby.id
	}
	w.u(lid)
	w.str(p.kind)
	return w.b
}

func lobbyMsg(l *lobby) []byte {
	var w writer
	w.u(l.id)
	w.u(l.host.id)
	w.str(l.mapname)
	w.s(l.mode)
	w.u(uint64(len(l.members)))
	for _, m := range l.members {
		w.u(m.id)
	}
	w.str(l.kind)
	return w.b
}

func (h *Hub) welcome(p *player, refusal string) {
	var w writer
	w.u(p.id)
	w.str(p.name)
	w.str(refusal)
	h.send(p, MsgWelcome, w.b)
}

func (h *Hub) join(p *player, want string) {
	if old := h.byKey[p.key]; old != nil {
		h.kick(old, "you connected from somewhere else")
		h.leave(old)
	}
	h.nextID++
	p.id = h.nextID
	h.players[p.id] = p
	h.byKey[p.key] = p
	refusal := h.rename(p, want)
	h.welcome(p, refusal)
	if h.witness != nil {
		h.send(p, MsgWitness, h.witness)
	}
	for _, q := range h.players {
		if q.name != "" && q != p {
			h.send(p, MsgPlayer, h.playerMsg(q))
		}
	}
	for _, l := range h.lobbies {
		h.send(p, MsgLobby, lobbyMsg(l))
	}
	if p.name != "" {
		h.broadcast(MsgPlayer, h.playerMsg(p))
	}
}

func (h *Hub) leave(p *player) {
	if h.players[p.id] != p {
		return
	}
	h.leaveLobby(p)
	delete(h.players, p.id)
	if h.byKey[p.key] == p {
		delete(h.byKey, p.key)
	}
	if p.name != "" {
		var w writer
		w.u(p.id)
		h.broadcast(MsgGone, w.b)
	}
}

// validName: 1 to 15 printable characters (UTF-8, as the game's names allow), no spaces.
func validName(n string) bool {
	if !utf8.ValidString(n) {
		return false
	}
	count := 0
	for _, r := range n {
		if !unicode.IsPrint(r) || unicode.IsSpace(r) {
			return false
		}
		count++
	}
	return count >= 1 && count <= 15
}

// rename gives p the name want if it can and returns why not otherwise.
func (h *Hub) rename(p *player, want string) string {
	if want == "" || want == p.name {
		return ""
	}
	if !validName(want) {
		return "a name is 1 to 15 characters, without spaces"
	}
	switch err := h.st.setName(p.key, want); {
	case errors.Is(err, errNameTaken):
		return fmt.Sprintf("the name %s is taken", want)
	case err != nil:
		return "the hub had a problem, try later"
	}
	p.name = want
	return ""
}

func (h *Hub) handle(p *player, typ byte, msg []byte) error {
	r := reader{b: msg}
	now := h.cfg.Now()
	switch typ {
	case MsgSetName:
		want := r.str(64)
		if err := r.done(); err != nil {
			return err
		}
		if !p.talk.take(now, TalkRate, TalkBurst) {
			h.notice(p, "slow down")
			return nil
		}
		had := p.name
		refusal := h.rename(p, want)
		h.welcome(p, refusal)
		if p.name != had {
			h.broadcast(MsgPlayer, h.playerMsg(p))
		}
	case MsgStatus:
		state, mapname, mode, kind := r.u(), r.str(64), r.s(), r.str(16)
		if err := r.done(); err != nil {
			return err
		}
		if state > StateRun {
			return errShort
		}
		if int(state) == p.state && mapname == p.mapname && mode == p.mode && kind == p.kind {
			return nil
		}
		p.state, p.mapname, p.mode, p.kind = int(state), mapname, mode, kind
		if p.name != "" {
			h.broadcast(MsgPlayer, h.playerMsg(p))
		}
	case MsgLobbyCreate:
		mapname, mode, kind := r.str(64), r.s(), r.str(16)
		if err := r.done(); err != nil {
			return err
		}
		if !h.named(p) {
			return nil
		}
		if !p.talk.take(now, TalkRate, TalkBurst) {
			h.notice(p, "slow down")
			return nil
		}
		if len(h.lobbies) >= MaxLobbies {
			h.notice(p, "there are too many lobbies right now")
			return nil
		}
		h.leaveLobby(p)
		h.nextLobby++
		l := &lobby{id: h.nextLobby, host: p, mapname: mapname, mode: mode, kind: kind, members: []*player{p}}
		h.lobbies[l.id] = l
		p.lobby = l
		h.broadcast(MsgLobby, lobbyMsg(l))
		h.broadcast(MsgPlayer, h.playerMsg(p))
	case MsgLobbyJoin:
		id := r.u()
		if err := r.done(); err != nil {
			return err
		}
		if !h.named(p) {
			return nil
		}
		l := h.lobbies[id]
		switch {
		case l == nil:
			h.notice(p, "that lobby is gone")
		case l == p.lobby:
		case len(l.members) >= MaxLobbyMembers:
			h.notice(p, "that lobby is full")
		default:
			h.leaveLobby(p)
			l.members = append(l.members, p)
			p.lobby = l
			h.broadcast(MsgLobby, lobbyMsg(l))
			h.broadcast(MsgPlayer, h.playerMsg(p))
		}
	case MsgLobbyLeave:
		if err := r.done(); err != nil {
			return err
		}
		if p.lobby != nil {
			h.leaveLobby(p)
			h.broadcast(MsgPlayer, h.playerMsg(p))
		}
	case MsgLobbyMap:
		mapname, mode, kind := r.str(64), r.s(), r.str(16)
		if err := r.done(); err != nil {
			return err
		}
		l := p.lobby
		if l == nil || l.host != p {
			h.notice(p, "only the lobby's host picks the map")
			return nil
		}
		l.mapname, l.mode, l.kind = mapname, mode, kind
		h.broadcast(MsgLobby, lobbyMsg(l))
	case MsgChat:
		scope, text := r.u(), r.str(4*MaxChat)
		if err := r.done(); err != nil {
			return err
		}
		h.chat(p, int(scope), text)
	case MsgReport:
		id, reason := r.u(), r.str(4*MaxChat)
		if err := r.done(); err != nil {
			return err
		}
		if !h.named(p) {
			return nil
		}
		if !p.talk.take(now, TalkRate, TalkBurst) {
			h.notice(p, "slow down")
			return nil
		}
		q := h.players[id]
		if q == nil || q.name == "" {
			h.notice(p, "that player is gone")
			return nil
		}
		if err := h.st.report(now.Unix(), p.key, q.key, q.name, cleanText(reason)); err != nil {
			return err
		}
		h.notice(p, fmt.Sprintf("reported %s, thanks", q.name))
	case MsgFinish:
		mapname, mode, kind, ms := r.str(64), r.s(), r.str(16), r.u()
		if err := r.done(); err != nil {
			return err
		}
		if p.lobby == nil || p.name == "" {
			return nil
		}
		var w writer
		w.u(p.id)
		w.str(p.name)
		w.str(mapname)
		w.s(mode)
		w.str(kind)
		w.u(ms)
		for _, q := range p.lobby.members {
			if q != p {
				h.send(q, MsgFinished, w.b)
			}
		}
	default:
		// messages from newer clients are ignored
	}
	return nil
}

func (h *Hub) named(p *player) bool {
	if p.name == "" {
		h.notice(p, "pick a name first")
		return false
	}
	return true
}

func (h *Hub) leaveLobby(p *player) {
	l := p.lobby
	if l == nil {
		return
	}
	p.lobby = nil
	for i, m := range l.members {
		if m == p {
			l.members = append(l.members[:i], l.members[i+1:]...)
			break
		}
	}
	if len(l.members) == 0 {
		delete(h.lobbies, l.id)
		var w writer
		w.u(l.id)
		h.broadcast(MsgLobbyGone, w.b)
		return
	}
	if l.host == p {
		l.host = l.members[0]
	}
	h.broadcast(MsgLobby, lobbyMsg(l))
}

// relayPos passes a position sample on to the other lobby members on the same map and mode.
// Samples over the rate, malformed ones and ones from outside a lobby are dropped.
func (h *Hub) relayPos(p *player, msg []byte) {
	if p.lobby == nil || p.name == "" || len(msg) > 48 || !p.pos.take(h.cfg.Now(), PosRate, PosBurst) {
		return
	}
	r := reader{b: msg}
	for i := 0; i < 6; i++ {
		r.u()
	}
	if r.done() != nil {
		return
	}
	var w writer
	w.u(p.id)
	w.bytes(msg)
	for _, q := range p.lobby.members {
		if q != p && q.mapname == p.mapname && q.mode == p.mode && q.state != StateMenu {
			h.send(q, MsgPosFrom, w.b)
		}
	}
}

// cleanText keeps valid UTF-8 without control characters (colour codes included), trims
// spaces and caps the length at MaxChat characters.
func cleanText(s string) string {
	var b strings.Builder
	n := 0
	for _, r := range strings.ToValidUTF8(s, "") {
		if unicode.IsControl(r) {
			continue
		}
		if n == MaxChat {
			break
		}
		b.WriteRune(r)
		n++
	}
	return strings.TrimSpace(b.String())
}

func (h *Hub) chat(p *player, scope int, text string) {
	if !h.named(p) {
		return
	}
	text = cleanText(text)
	if text == "" {
		return
	}
	now := h.cfg.Now()
	if p.admin && strings.HasPrefix(text, "#") {
		h.command(p, text)
		return
	}
	if !p.talk.take(now, TalkRate, TalkBurst) {
		h.notice(p, "slow down")
		return
	}
	if p.mutedUntil > now.Unix() {
		h.notice(p, "you are muted")
		return
	}
	var to []*player
	var lid uint64
	switch scope {
	case ChatGlobal:
		for _, q := range h.players {
			if q.name != "" {
				to = append(to, q)
			}
		}
	case ChatLobby:
		if p.lobby == nil {
			h.notice(p, "you aren't in a lobby")
			return
		}
		to, lid = p.lobby.members, p.lobby.id
	default:
		return
	}
	if err := h.st.logChat(now.Unix(), p.key, p.name, scope, lid, text); err != nil {
		h.cfg.Logf("chat log: %v", err)
	}
	var w writer
	w.u(uint64(scope))
	w.u(p.id)
	w.str(p.name)
	w.str(text)
	for _, q := range to {
		h.send(q, MsgChatFrom, w.b)
	}
}

// command runs an admin's "#mute <name> [minutes]", "#unmute <name>" or "#kick <name>".
func (h *Hub) command(p *player, text string) {
	f := strings.Fields(text)
	if len(f) < 2 {
		h.notice(p, "#mute <name> [minutes], #unmute <name>, #kick <name>")
		return
	}
	key, err := h.st.keyForName(f[1])
	if err != nil || key == "" {
		h.notice(p, "no player named "+f[1])
		return
	}
	target := h.byKey[key]
	now := h.cfg.Now()
	switch f[0] {
	case "#mute", "#unmute":
		until := int64(0)
		if f[0] == "#mute" {
			minutes := 60
			if len(f) > 2 {
				if m, err := strconv.Atoi(f[2]); err == nil && m > 0 {
					minutes = m
				}
			}
			until = now.Add(time.Duration(minutes) * time.Minute).Unix()
		}
		if err := h.st.mute(key, until); err != nil {
			h.notice(p, "the hub had a problem")
			return
		}
		if target != nil {
			target.mutedUntil = until
			if until > 0 {
				h.notice(target, "you are muted")
			} else {
				h.notice(target, "you can chat again")
			}
		}
		h.notice(p, f[0][1:]+"d "+f[1])
	case "#kick":
		if target == nil {
			h.notice(p, f[1]+" isn't online")
			return
		}
		h.kick(target, "you were kicked")
		h.leave(target)
		h.notice(p, "kicked "+f[1])
	default:
		h.notice(p, "#mute <name> [minutes], #unmute <name>, #kick <name>")
	}
}

// SetWitness tells every player, now and later, where the witness is and its keys.
func (h *Hub) SetWitness(host string, port int, channel, signing []byte) {
	var w writer
	w.str(host)
	w.u(uint64(port))
	w.bytes(channel)
	w.bytes(signing)
	h.mu.Lock()
	defer h.mu.Unlock()
	if string(w.b) == string(h.witness) {
		return
	}
	h.witness = w.b
	h.broadcast(MsgWitness, w.b)
}

// Prune drops chat lines older than the retention.
func (h *Hub) Prune() (int, error) {
	return h.st.pruneChat(h.cfg.Now().Add(-chatKeep).Unix())
}
