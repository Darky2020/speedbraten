// hub is the speedbraten hub server: names, presence, lobbies and chat (package hub).
//
//	hub -d <data dir> [-listen :28791] [-witness <host:port players use>] [-witnessask <host:port>] [-admins <file>]
//
// The data dir holds hub.key (the channel's static private key, made on first start) and
// hub.db. With -witness, the hub asks that witness (or -witnessask, when it is reached by
// another address from here) for its keys and hands address and keys to every player. The
// admins file lists install keys (hex, one per line, # comments) allowed to #mute and #kick.
package main

import (
	"bufio"
	"encoding/hex"
	"errors"
	"flag"
	"io/fs"
	"log"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/flynn/noise"

	"speedbraten/server/hub"
	"speedbraten/server/noisechan"
)

func loadKey(path string) (noise.DHKey, error) {
	priv, err := os.ReadFile(path)
	if errors.Is(err, fs.ErrNotExist) {
		k, err := noisechan.GenerateKey()
		if err != nil {
			return k, err
		}
		return k, os.WriteFile(path, k.Private, 0600)
	}
	if err != nil {
		return noise.DHKey{}, err
	}
	return noisechan.KeyFromPrivate(priv)
}

func loadAdmins(path string) (map[string]bool, error) {
	admins := map[string]bool{}
	if path == "" {
		return admins, nil
	}
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	s := bufio.NewScanner(f)
	for s.Scan() {
		line := strings.TrimSpace(s.Text())
		if i := strings.IndexByte(line, '#'); i >= 0 {
			line = strings.TrimSpace(line[:i])
		}
		if line == "" {
			continue
		}
		if b, err := hex.DecodeString(line); err != nil || len(b) != 32 {
			return nil, errors.New("admins: not a 64-digit hex key: " + line)
		}
		admins[strings.ToLower(line)] = true
	}
	return admins, s.Err()
}

func watchWitness(h *hub.Hub, advertised, ask string) {
	host, portstr, err := net.SplitHostPort(advertised)
	if err != nil {
		log.Fatalf("-witness: %v", err)
	}
	port, err := strconv.Atoi(portstr)
	if err != nil {
		log.Fatalf("-witness: %v", err)
	}
	known := false
	for {
		channel, signing, err := hub.WitnessKeys(ask, 10*time.Second)
		if err != nil {
			log.Printf("witness %s: %v", ask, err)
		} else {
			if !known {
				log.Printf("witness %s: channel key %x, signing key %x", advertised, channel, signing)
			}
			known = true
			h.SetWitness(host, port, channel, signing)
		}
		if known {
			time.Sleep(10 * time.Minute)
		} else {
			time.Sleep(time.Minute)
		}
	}
}

func main() {
	dir := flag.String("d", ".", "data dir (hub.key, hub.db)")
	listen := flag.String("listen", ":28791", "address to listen on")
	witness := flag.String("witness", "", "the witness's address as players reach it, host:port")
	ask := flag.String("witnessask", "", "where the hub reaches the witness, if not at -witness")
	admins := flag.String("admins", "", "file of admin install keys")
	flag.Parse()

	key, err := loadKey(filepath.Join(*dir, "hub.key"))
	if err != nil {
		log.Fatal(err)
	}
	adm, err := loadAdmins(*admins)
	if err != nil {
		log.Fatal(err)
	}
	h, err := hub.New(hub.Config{Key: key, DB: filepath.Join(*dir, "hub.db"), Admins: adm})
	if err != nil {
		log.Fatal(err)
	}
	ln, err := net.Listen("tcp", *listen)
	if err != nil {
		log.Fatal(err)
	}
	log.Printf("hub listening on %s, key %x, %d admins", ln.Addr(), key.Public, len(adm))
	if *witness != "" {
		if *ask == "" {
			*ask = *witness
		}
		go watchWitness(h, *witness, *ask)
	}
	go func() {
		for {
			if n, err := h.Prune(); err != nil {
				log.Printf("prune: %v", err)
			} else if n > 0 {
				log.Printf("pruned %d chat lines", n)
			}
			time.Sleep(time.Hour)
		}
	}()
	for {
		c, err := ln.Accept()
		if err != nil {
			var ne net.Error
			if errors.As(err, &ne) && ne.Timeout() {
				continue
			}
			log.Fatal(err)
		}
		addr, _, _ := net.SplitHostPort(c.RemoteAddr().String())
		go h.Serve(c, addr)
	}
}
