// site serves the public leaderboards (and, under /admin, the dashboard) over HTTP for a proxy
// such as nginx to put behind HTTPS.
//
//	site -witnessdb <witness.db> -hubdb <hub.db> -sitedb <site.db> [-listen 127.0.0.1:8792]
package main

import (
	"flag"
	"log"
	"net/http"
	"time"

	"speedbraten/server/board"
)

func main() {
	witnessDB := flag.String("witnessdb", "", "the witness's database (read only)")
	hubDB := flag.String("hubdb", "", "the hub's database, for names (read only)")
	siteDB := flag.String("sitedb", "site.db", "the site's own database: moderation")
	listen := flag.String("listen", "127.0.0.1:8792", "address to serve HTTP on")
	flag.Parse()
	if *witnessDB == "" {
		log.Fatal("-witnessdb is needed")
	}
	store, err := board.Open(*witnessDB, *hubDB, *siteDB, 30*time.Second)
	if err != nil {
		log.Fatal(err)
	}
	defer store.Close()
	srv := &http.Server{Addr: *listen, Handler: newSite(store), ReadHeaderTimeout: 10 * time.Second}
	log.Printf("site on http://%s", *listen)
	log.Fatal(srv.ListenAndServe())
}
