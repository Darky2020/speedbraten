// Package noisechan is the encrypted channel between speedbraten clients and its servers:
// Noise_XK_25519_ChaChaPoly_BLAKE2b over a byte stream. The client (initiator) knows the
// server's static key in advance; the server learns the client's static key in the third
// handshake message.
//
// Framing: every Noise message is sent as a big-endian u16 length followed by the message.
// A transport message's plaintext is a u8 type followed by the payload.
package noisechan

import (
	"crypto/rand"
	"encoding/binary"
	"errors"
	"fmt"
	"io"

	"github.com/flynn/noise"
)

// MaxPayload is the largest payload one transport message carries.
const MaxPayload = noise.MaxMsgLen - 16 - 1

var Suite = noise.NewCipherSuite(noise.DH25519, noise.CipherChaChaPoly, noise.HashBLAKE2b)

// Conn is an established channel. It is not safe for concurrent writes or concurrent reads.
type Conn struct {
	rw         io.ReadWriter
	send, recv *noise.CipherState
	peer       []byte
	binding    []byte
	buf        []byte
}

// PeerStatic is the remote side's static public key.
func (c *Conn) PeerStatic() []byte { return c.peer }

// ChannelBinding is the handshake hash, the same on both sides of one channel.
func (c *Conn) ChannelBinding() []byte { return c.binding }

// GenerateKey returns a new static key pair.
func GenerateKey() (noise.DHKey, error) { return noise.DH25519.GenerateKeypair(rand.Reader) }

// KeyFromPrivate builds a key pair from a 32-byte private key.
func KeyFromPrivate(priv []byte) (noise.DHKey, error) {
	if len(priv) != 32 {
		return noise.DHKey{}, errors.New("noisechan: private key must be 32 bytes")
	}
	pub, err := noise.DH25519.DH(priv, basepoint)
	if err != nil {
		return noise.DHKey{}, err
	}
	return noise.DHKey{Private: append([]byte(nil), priv...), Public: pub}, nil
}

var basepoint = func() []byte { b := make([]byte, 32); b[0] = 9; return b }()

func writeFrame(w io.Writer, msg []byte) error {
	if len(msg) > noise.MaxMsgLen {
		return errors.New("noisechan: message too long")
	}
	frame := make([]byte, 2+len(msg))
	binary.BigEndian.PutUint16(frame, uint16(len(msg)))
	copy(frame[2:], msg)
	_, err := w.Write(frame)
	return err
}

func readFrame(r io.Reader, buf []byte) ([]byte, error) {
	var hdr [2]byte
	if _, err := io.ReadFull(r, hdr[:]); err != nil {
		return nil, err
	}
	n := int(binary.BigEndian.Uint16(hdr[:]))
	if cap(buf) < n {
		buf = make([]byte, n)
	}
	buf = buf[:n]
	if _, err := io.ReadFull(r, buf); err != nil {
		return nil, err
	}
	return buf, nil
}

func handshake(rw io.ReadWriter, cfg noise.Config, payload []byte) (*Conn, []byte, error) {
	cfg.CipherSuite = Suite
	cfg.Pattern = noise.HandshakeXK
	if cfg.Random == nil {
		cfg.Random = rand.Reader
	}
	hs, err := noise.NewHandshakeState(cfg)
	if err != nil {
		return nil, nil, err
	}
	var got []byte
	for i := 0; i < 3; i++ {
		var cs1, cs2 *noise.CipherState
		if (i%2 == 0) == cfg.Initiator {
			var msg []byte
			msg, cs1, cs2, err = hs.WriteMessage(nil, payloadFor(i, cfg.Initiator, payload))
			if err == nil {
				err = writeFrame(rw, msg)
			}
		} else {
			var msg, p []byte
			msg, err = readFrame(rw, nil)
			if err == nil {
				p, cs1, cs2, err = hs.ReadMessage(nil, msg)
				if len(p) > 0 {
					got = p
				}
			}
		}
		if err != nil {
			return nil, nil, fmt.Errorf("noisechan: handshake message %d: %w", i+1, err)
		}
		if cs1 != nil {
			c := &Conn{rw: rw, peer: hs.PeerStatic(), binding: hs.ChannelBinding()}
			if cfg.Initiator {
				c.send, c.recv = cs1, cs2
			} else {
				c.send, c.recv = cs2, cs1
			}
			return c, got, nil
		}
	}
	return nil, nil, errors.New("noisechan: handshake did not finish")
}

// The initiator's payload goes in message 3, the responder's in message 2: both are then
// encrypted to a key the other side has authenticated.
func payloadFor(i int, initiator bool, payload []byte) []byte {
	if (initiator && i == 2) || (!initiator && i == 1) {
		return payload
	}
	return nil
}

// Client runs the handshake as the initiator. serverStatic is the pinned server key; payload
// travels in the third message and the server's reply payload is returned.
func Client(rw io.ReadWriter, static noise.DHKey, serverStatic, prologue, payload []byte) (*Conn, []byte, error) {
	return handshake(rw, noise.Config{Initiator: true, StaticKeypair: static, PeerStatic: serverStatic, Prologue: prologue}, payload)
}

// Server runs the handshake as the responder; payload travels in the second message. The
// client's payload is returned with the channel.
func Server(rw io.ReadWriter, static noise.DHKey, prologue, payload []byte) (*Conn, []byte, error) {
	return handshake(rw, noise.Config{StaticKeypair: static, Prologue: prologue}, payload)
}

// Write sends one transport message.
func (c *Conn) Write(typ byte, payload []byte) error {
	if len(payload) > MaxPayload {
		return errors.New("noisechan: payload too long")
	}
	plain := make([]byte, 1+len(payload))
	plain[0] = typ
	copy(plain[1:], payload)
	msg, err := c.send.Encrypt(nil, nil, plain)
	if err != nil {
		return err
	}
	return writeFrame(c.rw, msg)
}

// Read receives one transport message. Any error ends the channel: the caller closes it.
func (c *Conn) Read() (byte, []byte, error) {
	msg, err := readFrame(c.rw, c.buf)
	if err != nil {
		return 0, nil, err
	}
	c.buf = msg
	plain, err := c.recv.Decrypt(nil, nil, msg)
	if err != nil {
		return 0, nil, err
	}
	if len(plain) == 0 {
		return 0, nil, errors.New("noisechan: empty transport message")
	}
	return plain[0], plain[1:], nil
}
