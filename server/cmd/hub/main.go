// hub is the speedbraten hub server: names, presence, lobbies and chat (package hub).
//
//	hub -d <data dir> [-listen :28791] [-witness <host:port players use>] [-witnessask <host:port>]
//	    [-witnessdb <witness.db>] [-admins <file>] [-discordbot <bot token file> -discord <channel ids | @file>]
//	    [-releases <GitHub latest-release API URL>]
//
// The data dir holds hub.key (the channel's static private key, made on first start) and
// hub.db. With -witness, the hub asks that witness (or -witnessask, when it is reached by
// another address from here) for its keys and hands address and keys to every player.
// With -witnessdb (on the witness's machine) it announces PBs and records to everyone. The
// admins file lists install keys (hex, one per line, # comments) allowed to #mute and #kick.
// With -discordbot and -discord, the bot posts joins, leaves, chat and records to each of those
// Discord channels and keeps a message there with the lobbies up to date, and what people post
// there is shown to every player. The bot needs the Message Content intent, and View Channel,
// Send Messages and Read Message History in the channels. Every 30 minutes the hub asks GitHub
// for the newest client release and tells players, whose clients say when theirs is older.
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

	"speedbraten/server/board"
	"speedbraten/server/discord"
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

// readList is the comma-separated values of v, or with "@path" the lines of that file (blank lines
// and # comments left out)
func readList(v string) ([]string, error) {
	text, sep := v, ","
	if strings.HasPrefix(v, "@") {
		b, err := os.ReadFile(v[1:])
		if err != nil {
			return nil, err
		}
		text, sep = string(b), "\n"
	}
	var out []string
	for _, s := range strings.Split(text, sep) {
		if s = strings.TrimSpace(s); s != "" && !strings.HasPrefix(s, "#") {
			out = append(out, s)
		}
	}
	if len(out) == 0 {
		return nil, errors.New(v + " lists nothing")
	}
	return out, nil
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
			time.Sleep(10 * time.Second)
		}
	}
}

func main() {
	dir := flag.String("d", ".", "data dir (hub.key, hub.db)")
	listen := flag.String("listen", ":28791", "address to listen on")
	witness := flag.String("witness", "", "the witness's address as players reach it, host:port")
	ask := flag.String("witnessask", "", "where the hub reaches the witness, if not at -witness")
	admins := flag.String("admins", "", "file of admin install keys")
	witnessDB := flag.String("witnessdb", "", "the witness's witness.db, to announce PBs and records and serve the boards")
	siteDB := flag.String("sitedb", "", "the leaderboard site's site.db (its moderation); default <data dir>/site.db")
	maps := flag.String("maps", "", "game data dir whose packages/base maps players may download (the witness's)")
	raceMaps := flag.String("racemaps", "", "package dir of the race maps (<dir>/packages/base), listed and downloadable")
	legacy := flag.String("legacy", "", "dir of the legacy flagrun records' ghosts and cut demos (tools/legacyflagruns.py --out)")
	releases := flag.String("releases", "https://api.github.com/repos/Darky2020/speedbraten/releases/latest", "GitHub API URL of the newest client release, \"\" for none")
	discordBot := flag.String("discordbot", "", "file holding the Discord bot's token")
	discordChannels := flag.String("discord", "", "Discord channel ids (comma-separated, or @file: one per line) for joins, leaves, chat, records and the lobbies, and whose messages players see")
	flag.Parse()

	key, err := loadKey(filepath.Join(*dir, "hub.key"))
	if err != nil {
		log.Fatal(err)
	}
	adm, err := loadAdmins(*admins)
	if err != nil {
		log.Fatal(err)
	}
	cfg := hub.Config{Key: key, DB: filepath.Join(*dir, "hub.db"), Admins: adm, Maps: *maps, RaceMaps: *raceMaps, Legacy: *legacy}
	if *raceMaps != "" {
		cfg.Thumbs = filepath.Join(*dir, "thumbs")
	}
	var discordToken string
	var channels []string
	if *discordBot != "" || *discordChannels != "" {
		token, err := os.ReadFile(*discordBot)
		if err != nil {
			log.Fatalf("-discordbot: %v", err)
		}
		if channels, err = readList(*discordChannels); err != nil {
			log.Fatalf("-discord: %v", err)
		}
		discordToken = strings.TrimSpace(string(token))
		var all discord.All
		for _, ch := range channels {
			all = append(all, discord.New(discordToken, ch, log.Printf))
		}
		cfg.Feed = all
		log.Printf("discord: feed to and chat from %d channels", len(channels))
	}
	h, err := hub.New(cfg)
	if err != nil {
		log.Fatal(err)
	}
	if *releases != "" {
		go h.ReleaseLoop(*releases, 30*time.Minute, log.Printf)
	}
	for _, ch := range channels {
		r := discord.NewReader(discordToken, ch, 2*time.Second, log.Printf)
		go r.Run(nil, func(m discord.Message) { h.DiscordSay(m.AuthorID, m.Author, m.Text) })
	}
	go func() {
		for {
			h.PresenceTick()
			time.Sleep(2 * time.Second)
		}
	}()
	if cfg.Feed != nil {
		go func() {
			for {
				h.FeedTick()
				time.Sleep(5 * time.Second)
			}
		}()
	}
	if *raceMaps != "" {
		go func() {
			for {
				start := time.Now()
				if n := h.RefreshMapCRCs(); n > 0 {
					log.Printf("race maps: checksums of %d maps in %v", n, time.Since(start).Round(time.Millisecond))
				}
				time.Sleep(time.Minute)
			}
		}()
		go h.ThumbLoop(log.Printf)
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
	if *witnessDB != "" {
		res, err := h.OpenResults(*witnessDB)
		if err != nil {
			log.Fatalf("-witnessdb: %v", err)
		}
		if *siteDB == "" {
			*siteDB = filepath.Join(*dir, "site.db")
		}
		boards, err := board.Open(*witnessDB, filepath.Join(*dir, "hub.db"), *siteDB, 30*time.Second)
		if err != nil {
			log.Fatalf("boards: %v", err)
		}
		h.SetBoards(boards)
		go func() {
			for {
				if n, err := res.Poll(); err != nil {
					log.Printf("witness results: %v", err)
				} else if n > 0 {
					log.Printf("announced %d results", n)
				}
				time.Sleep(time.Second)
			}
		}()
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
