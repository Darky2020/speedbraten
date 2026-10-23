package hub

import (
	"encoding/binary"
	"errors"
)

// The numbers in src/shared/hubproto.h.
const (
	Proto = 1

	MsgHello       = 1
	MsgSetName     = 2
	MsgStatus      = 3
	MsgLobbyCreate = 4
	MsgLobbyJoin   = 5
	MsgLobbyLeave  = 6
	MsgLobbyMap    = 7
	MsgChat        = 8
	MsgReport      = 9

	MsgWelcome   = 101
	MsgWitness   = 102
	MsgPlayer    = 103
	MsgGone      = 104
	MsgLobby     = 105
	MsgLobbyGone = 106
	MsgChatFrom  = 107
	MsgNotice    = 108

	StateMenu    = 0
	StatePlaying = 1
	StateRun     = 2

	ChatGlobal = 0
	ChatLobby  = 1

	Prologue = "speedbraten/1 hub"
)

type writer struct{ b []byte }

func (w *writer) u(v uint64)     { w.b = binary.AppendUvarint(w.b, v) }
func (w *writer) s(v int64)      { w.b = binary.AppendVarint(w.b, v) }
func (w *writer) bytes(p []byte) { w.b = append(w.b, p...) }
func (w *writer) str(v string)   { w.u(uint64(len(v))); w.b = append(w.b, v...) }

var errShort = errors.New("hub: malformed message")

// reader keeps the first error; every read after it returns zero values.
type reader struct {
	b   []byte
	err error
}

func (r *reader) u() uint64 {
	if r.err != nil {
		return 0
	}
	v, n := binary.Uvarint(r.b)
	// a non-minimal varint ends in a zero byte past the first one
	if n <= 0 || (n > 1 && r.b[n-1] == 0) {
		r.err = errShort
		return 0
	}
	r.b = r.b[n:]
	return v
}

func (r *reader) s() int64 {
	v := r.u()
	return int64(v>>1) ^ -int64(v&1)
}

func (r *reader) str(max int) string {
	n := r.u()
	if r.err != nil {
		return ""
	}
	if n > uint64(len(r.b)) || n > uint64(max) {
		r.err = errShort
		return ""
	}
	v := string(r.b[:n])
	r.b = r.b[n:]
	return v
}

func (r *reader) done() error {
	if r.err == nil && len(r.b) != 0 {
		r.err = errShort
	}
	return r.err
}
