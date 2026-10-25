// site serves the public leaderboards, and under /admin the dashboard, over HTTP for a proxy such
// as nginx to put behind HTTPS.
//
//	site setpass <password file>
//	    reads the dashboard password on stdin and writes its bcrypt hash (mode 0600)
//	site -witnessdb <witness.db> -hubdb <hub.db> -sitedb <site.db> [-pass <password file>]
//	     [-rejudgedir <witness data dir>/rejudge] [-listen 127.0.0.1:8792]
//	    without -pass there is no dashboard
package main

import (
	"bufio"
	"flag"
	"fmt"
	"log"
	"net/http"
	"os"
	"strings"
	"time"

	"golang.org/x/crypto/bcrypt"

	"speedbraten/server/board"
)

func setpass(file string) {
	fmt.Fprint(os.Stderr, "password: ")
	line, err := bufio.NewReader(os.Stdin).ReadString('\n')
	pass := strings.TrimRight(line, "\r\n")
	if (err != nil && line == "") || len(pass) < 12 {
		log.Fatal("a password of at least 12 characters is needed")
	}
	hash, err := bcrypt.GenerateFromPassword([]byte(pass), 12)
	if err != nil {
		log.Fatal(err)
	}
	if err := os.WriteFile(file, append(hash, '\n'), 0600); err != nil {
		log.Fatal(err)
	}
}

func main() {
	if len(os.Args) == 3 && os.Args[1] == "setpass" {
		setpass(os.Args[2])
		return
	}
	witnessDB := flag.String("witnessdb", "", "the witness's database (read only)")
	hubDB := flag.String("hubdb", "", "the hub's database, for names (read only)")
	siteDB := flag.String("sitedb", "site.db", "the site's own database: moderation")
	listen := flag.String("listen", "127.0.0.1:8792", "address to serve HTTP on")
	passfile := flag.String("pass", "", "the dashboard's password hash (site setpass); no dashboard without it")
	rejudgeDir := flag.String("rejudgedir", "", "the witness's <data dir>/rejudge, for re-judging runs from the dashboard")
	shots := flag.String("mapshots", "", "pictures of maps chosen by hand: <map>.jpg or <map>.png")
	previews := flag.String("mappreviews", "", "the game's packages/base, whose <map>.jpg previews stand in for maps without a picture")
	hubAddr := flag.String("hubaddr", "141.95.67.191", "the hub's address as the info page gives it")
	flag.Parse()
	if *witnessDB == "" {
		log.Fatal("-witnessdb is needed")
	}
	store, err := board.Open(*witnessDB, *hubDB, *siteDB, 30*time.Second)
	if err != nil {
		log.Fatal(err)
	}
	defer store.Close()
	s := newSite(store)
	s.shots, s.previews, s.hubAddr = *shots, *previews, *hubAddr
	if *passfile != "" {
		hash, err := os.ReadFile(*passfile)
		if err != nil {
			log.Fatal(err)
		}
		newAdmin(store, []byte(strings.TrimSpace(string(hash))), *rejudgeDir).register(s.mux)
	}
	srv := &http.Server{Addr: *listen, Handler: s, ReadHeaderTimeout: 10 * time.Second, ReadTimeout: 30 * time.Second, WriteTimeout: 120 * time.Second}
	log.Printf("site on http://%s", *listen)
	log.Fatal(srv.ListenAndServe())
}
