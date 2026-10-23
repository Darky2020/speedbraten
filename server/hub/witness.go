package hub

import (
	"bytes"
	"errors"
	"net"
	"time"

	"speedbraten/server/noisechan"
)

// WitnessKeys asks the witness at addr for its channel and signing public keys.
func WitnessKeys(addr string, timeout time.Duration) (channel, signing []byte, err error) {
	c, err := net.DialTimeout("tcp", addr, timeout)
	if err != nil {
		return nil, nil, err
	}
	defer c.Close()
	c.SetDeadline(time.Now().Add(timeout))
	reply, err := noisechan.RequestKeys(c)
	if err != nil {
		return nil, nil, err
	}
	if len(reply) != 5+64 || !bytes.Equal(reply[:5], []byte("SBRW\x01")) {
		return nil, nil, errors.New("hub: the witness didn't send its keys")
	}
	return reply[5:37], reply[37:69], nil
}
