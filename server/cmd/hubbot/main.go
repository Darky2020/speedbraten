// hubbot is a test player for the hub: it joins a named player's lobby and sends position
// samples at 20 a second, circling near a point, so that player's client draws its ghost.
//
//	hubbot -hub 127.0.0.1:28898 -key <hub public key hex> -follow alice -x 512 -y 512 -z 520
package main

import (
	"encoding/binary"
	"encoding/hex"
	"flag"
	"log"
	"math"
	"net"
	"time"

	"speedbraten/server/hub"
	"speedbraten/server/noisechan"
)

func str(b []byte, s string) []byte { b = binary.AppendUvarint(b, uint64(len(s))); return append(b, s...) }

func main() {
	addr := flag.String("hub", "127.0.0.1:28898", "hub address")
	keyhex := flag.String("key", "", "hub public key, hex")
	follow := flag.String("follow", "alice", "the player whose lobby to join")
	x, y, z := flag.Float64("x", 512, ""), flag.Float64("y", 512, ""), flag.Float64("z", 520, "")
	flag.Parse()
	pub, err := hex.DecodeString(*keyhex)
	if err != nil {
		log.Fatal(err)
	}
	key, _ := noisechan.GenerateKey()
	c, err := net.Dial("tcp", *addr)
	if err != nil {
		log.Fatal(err)
	}
	ch, _, err := noisechan.Client(c, key, pub, []byte(hub.Prologue), []byte("hubbot"))
	if err != nil {
		log.Fatal(err)
	}
	ch.Write(hub.MsgHello, str(nil, "bot"))
	players := map[uint64]string{}
	lobbies := map[uint64]uint64{} // player id -> lobby id
	go func() {
		for {
			typ, p, err := ch.Read()
			if err != nil {
				log.Fatal(err)
			}
			if typ == hub.MsgPlayer {
				id, n := binary.Uvarint(p)
				l, m := binary.Uvarint(p[n:])
				name := string(p[n+m : n+m+int(l)])
				players[id] = name
				rest := p[n+m+int(l):]
				_, a := binary.Uvarint(rest)       // state
				ml, b := binary.Uvarint(rest[a:])  // map
				rest = rest[a+b+int(ml):]
				_, d := binary.Varint(rest)        // mode
				lob, _ := binary.Uvarint(rest[d:]) // lobby
				lobbies[id] = lob
			}
		}
	}()
	status := binary.AppendUvarint(nil, hub.StatePlaying)
	status = str(status, "forge")
	status = binary.AppendVarint(status, 17)
	status = str(status, "flagrun")
	ch.Write(hub.MsgStatus, status)
	joined := uint64(0)
	start := time.Now()
	for range time.Tick(50 * time.Millisecond) {
		if joined == 0 {
			for id, name := range players {
				if name == *follow && lobbies[id] != 0 {
					joined = lobbies[id]
					ch.Write(hub.MsgLobbyJoin, binary.AppendUvarint(nil, joined))
					ch.Write(hub.MsgSpectate, binary.AppendUvarint(nil, id))
					log.Printf("joined lobby %d of %s and spectates", joined, name)
				}
			}
			continue
		}
		t := time.Since(start).Seconds()
		px, py := *x+60*math.Cos(t), *y+60*math.Sin(t)
		var b []byte
		b = binary.AppendVarint(b, int64(px*16))
		b = binary.AppendVarint(b, int64(py*16))
		b = binary.AppendVarint(b, int64(*z*16))
		b = binary.AppendUvarint(b, uint64(math.Mod(t*57, 360)/360*65536)&0xFFFF)
		b = binary.AppendUvarint(b, 32767)
		b = binary.AppendUvarint(b, 1<<0|1<<2)
		ch.Write(hub.MsgPos, b)
	}
}
