package noisechan

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"net"
	"os"
	"testing"

	"github.com/flynn/noise"
)

type vector struct {
	ProtocolName     string `json:"protocol_name"`
	InitPrologue     string `json:"init_prologue"`
	InitStatic       string `json:"init_static"`
	InitEphemeral    string `json:"init_ephemeral"`
	InitRemoteStatic string `json:"init_remote_static"`
	RespPrologue     string `json:"resp_prologue"`
	RespStatic       string `json:"resp_static"`
	RespEphemeral    string `json:"resp_ephemeral"`
	HandshakeHash    string `json:"handshake_hash"`
	Messages         []struct {
		Payload    string `json:"payload"`
		Ciphertext string `json:"ciphertext"`
	} `json:"messages"`
}

func unhex(t *testing.T, s string) []byte {
	b, err := hex.DecodeString(s)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func key(t *testing.T, s string) noise.DHKey {
	k, err := KeyFromPrivate(unhex(t, s))
	if err != nil {
		t.Fatal(err)
	}
	return k
}

func TestVectors(t *testing.T) {
	raw, err := os.ReadFile("../../tests/noise/xk-vectors.json")
	if err != nil {
		t.Fatal(err)
	}
	var file struct{ Vectors []vector }
	if err := json.Unmarshal(raw, &file); err != nil {
		t.Fatal(err)
	}
	if len(file.Vectors) == 0 {
		t.Fatal("no vectors")
	}
	for vi, v := range file.Vectors {
		if v.ProtocolName != "Noise_XK_25519_ChaChaPoly_BLAKE2b" {
			t.Fatalf("vector %d: unexpected protocol %s", vi, v.ProtocolName)
		}
		ini, err := noise.NewHandshakeState(noise.Config{CipherSuite: Suite, Pattern: noise.HandshakeXK, Initiator: true,
			Prologue: unhex(t, v.InitPrologue), StaticKeypair: key(t, v.InitStatic), PeerStatic: unhex(t, v.InitRemoteStatic),
			Random: bytes.NewReader(unhex(t, v.InitEphemeral))})
		if err != nil {
			t.Fatal(err)
		}
		res, err := noise.NewHandshakeState(noise.Config{CipherSuite: Suite, Pattern: noise.HandshakeXK,
			Prologue: unhex(t, v.RespPrologue), StaticKeypair: key(t, v.RespStatic), Random: bytes.NewReader(unhex(t, v.RespEphemeral))})
		if err != nil {
			t.Fatal(err)
		}
		var isend, irecv, rsend, rrecv *noise.CipherState
		for i, m := range v.Messages {
			payload, want := unhex(t, m.Payload), unhex(t, m.Ciphertext)
			fromIni := i%2 == 0
			var msg, got []byte
			if i < 3 {
				w, r := ini, res
				if !fromIni {
					w, r = res, ini
				}
				var c1, c2 *noise.CipherState
				msg, c1, c2, err = w.WriteMessage(nil, payload)
				if err != nil {
					t.Fatalf("vector %d message %d: %v", vi, i, err)
				}
				if c1 != nil {
					isend, irecv = c1, c2
				}
				got, c1, c2, err = r.ReadMessage(nil, msg)
				if err != nil {
					t.Fatalf("vector %d message %d: %v", vi, i, err)
				}
				if c1 != nil {
					rrecv, rsend = c1, c2
				}
			} else {
				w, r := isend, rrecv
				if !fromIni {
					w, r = rsend, irecv
				}
				if msg, err = w.Encrypt(nil, nil, payload); err == nil {
					got, err = r.Decrypt(nil, nil, msg)
				}
				if err != nil {
					t.Fatalf("vector %d message %d: %v", vi, i, err)
				}
			}
			if !bytes.Equal(msg, want) || !bytes.Equal(got, payload) {
				t.Fatalf("vector %d message %d: ciphertext or payload differs", vi, i)
			}
		}
		if v.HandshakeHash != "" && !bytes.Equal(ini.ChannelBinding(), unhex(t, v.HandshakeHash)) {
			t.Fatalf("vector %d: handshake hash differs", vi)
		}
	}
}

func pair(t *testing.T, serverKey noise.DHKey, pinned []byte) (*Conn, *Conn, []byte, []byte, error, error) {
	a, b := net.Pipe()
	t.Cleanup(func() { a.Close(); b.Close() })
	clientKey, _ := GenerateKey()
	prologue := []byte("speedbraten/1 test")
	type result struct {
		c   *Conn
		p   []byte
		err error
	}
	done := make(chan result, 1)
	go func() {
		c, p, err := Server(b, serverKey, prologue, []byte("server hello"))
		if err != nil {
			b.Close()
		}
		done <- result{c, p, err}
	}()
	cc, cp, cerr := Client(a, clientKey, pinned, prologue, []byte("client hello"))
	if cerr != nil {
		a.Close()
	}
	s := <-done
	if cerr == nil && s.err == nil && !bytes.Equal(s.c.PeerStatic(), clientKey.Public) {
		t.Fatal("server sees a different client key")
	}
	return cc, s.c, cp, s.p, cerr, s.err
}

func TestRoundTrip(t *testing.T) {
	serverKey, _ := GenerateKey()
	cc, sc, cp, sp, cerr, serr := pair(t, serverKey, serverKey.Public)
	if cerr != nil || serr != nil {
		t.Fatal(cerr, serr)
	}
	if string(cp) != "server hello" || string(sp) != "client hello" {
		t.Fatalf("payloads: %q %q", cp, sp)
	}
	if !bytes.Equal(cc.ChannelBinding(), sc.ChannelBinding()) {
		t.Fatal("channel bindings differ")
	}
	go func() {
		for i := 0; i < 100; i++ {
			cc.Write(byte(i), bytes.Repeat([]byte{byte(i)}, i*600))
		}
	}()
	for i := 0; i < 100; i++ {
		typ, p, err := sc.Read()
		if err != nil || typ != byte(i) || len(p) != i*600 {
			t.Fatalf("message %d: %v type %d len %d", i, err, typ, len(p))
		}
	}
}

func TestWrongServerKey(t *testing.T) {
	serverKey, _ := GenerateKey()
	other, _ := GenerateKey()
	_, _, _, _, cerr, serr := pair(t, serverKey, other.Public)
	if cerr == nil && serr == nil {
		t.Fatal("a handshake against the wrong pinned key succeeded")
	}
}

// tamper sends one transport message through a pipe, lets f change the framed bytes, and
// reports whether the receiver accepted the result
func tamper(t *testing.T, f func(frames [][]byte) [][]byte) bool {
	serverKey, _ := GenerateKey()
	a, b := net.Pipe()
	defer a.Close()
	defer b.Close()
	clientKey, _ := GenerateKey()
	done := make(chan *Conn, 1)
	go func() { c, _, _ := Server(b, serverKey, nil, nil); done <- c }()
	cc, _, err := Client(a, clientKey, serverKey.Public, nil, nil)
	sc := <-done
	if err != nil || sc == nil {
		t.Fatal("handshake failed")
	}
	var buf bytes.Buffer
	cc.rw = &buf
	cc.Write(1, []byte("first"))
	first := append([]byte(nil), buf.Bytes()...)
	buf.Reset()
	cc.Write(1, []byte("second"))
	second := append([]byte(nil), buf.Bytes()...)
	var in bytes.Buffer
	for _, fr := range f([][]byte{first, second}) {
		in.Write(fr)
	}
	sc.rw = &in
	for range f([][]byte{first, second}) {
		if _, _, err := sc.Read(); err != nil {
			return false
		}
	}
	return true
}

func TestTampering(t *testing.T) {
	if !tamper(t, func(fr [][]byte) [][]byte { return fr }) {
		t.Fatal("untouched messages were refused")
	}
	cases := map[string]func([][]byte) [][]byte{
		"flipped byte": func(fr [][]byte) [][]byte { b := append([]byte(nil), fr[0]...); b[5] ^= 1; return [][]byte{b} },
		"reordered":    func(fr [][]byte) [][]byte { return [][]byte{fr[1], fr[0]} },
		"replayed":     func(fr [][]byte) [][]byte { return [][]byte{fr[0], fr[0]} },
		"truncated":    func(fr [][]byte) [][]byte { b := append([]byte(nil), fr[0]...); b[1]--; return [][]byte{b[:len(b)-1]} },
	}
	for name, f := range cases {
		if tamper(t, f) {
			t.Errorf("%s message was accepted", name)
		}
	}
}
