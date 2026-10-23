// noiseecho is the far end of the client's noiseinterop check: it accepts noisechan
// connections (service "test") and sends every transport message back unchanged.
//
//	noiseecho -listen 127.0.0.1:28800 [-key <64 hex digits of the static private key>]
//
// It prints its static public key, which noiseinterop takes as the pinned server key.
package main

import (
	"encoding/hex"
	"flag"
	"fmt"
	"log"
	"net"

	"speedbraten/server/noisechan"
)

func main() {
	listen := flag.String("listen", "127.0.0.1:28800", "address to listen on")
	keyhex := flag.String("key", "", "static private key, hex (a new one if empty)")
	flag.Parse()
	key, err := noisechan.GenerateKey()
	if *keyhex != "" {
		var priv []byte
		if priv, err = hex.DecodeString(*keyhex); err == nil {
			key, err = noisechan.KeyFromPrivate(priv)
		}
	}
	if err != nil {
		log.Fatal(err)
	}
	ln, err := net.Listen("tcp", *listen)
	if err != nil {
		log.Fatal(err)
	}
	fmt.Printf("noiseecho listening on %s, key %x\n", ln.Addr(), key.Public)
	for {
		c, err := ln.Accept()
		if err != nil {
			log.Fatal(err)
		}
		go func(c net.Conn) {
			defer c.Close()
			ch, hello, err := noisechan.Server(c, key, []byte("speedbraten/1 test"), []byte("echo/1"))
			if err != nil {
				log.Printf("%s: %v", c.RemoteAddr(), err)
				return
			}
			n := 0
			for {
				typ, p, err := ch.Read()
				if err != nil {
					log.Printf("%s: client %x hello %q: %d messages echoed, then %v", c.RemoteAddr(), ch.PeerStatic(), hello, n, err)
					return
				}
				if err := ch.Write(typ, p); err != nil {
					log.Printf("%s: %v", c.RemoteAddr(), err)
					return
				}
				n++
			}
		}(c)
	}
}
