// witnesskeys makes and shows the witness's replay signing key (Ed25519, RFC 8032).
//
//	witnesskeys gen <seed file>   writes a new 32-byte seed as hex (mode 0600), prints the public key
//	witnesskeys show <seed file>  prints the public key of an existing seed
//
// The public key goes into the client's built-in key list and into witnesskeys.txt files.
package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"os"
	"strings"
)

func usage() {
	fmt.Fprintln(os.Stderr, "usage: witnesskeys gen|show <seed file>")
	os.Exit(2)
}

func main() {
	if len(os.Args) != 3 {
		usage()
	}
	file := os.Args[2]
	var seed []byte
	switch os.Args[1] {
	case "gen":
		seed = make([]byte, ed25519.SeedSize)
		if _, err := rand.Read(seed); err != nil {
			fail(err)
		}
		f, err := os.OpenFile(file, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
		if err != nil {
			fail(err)
		}
		if _, err := fmt.Fprintln(f, hex.EncodeToString(seed)); err != nil {
			fail(err)
		}
		if err := f.Close(); err != nil {
			fail(err)
		}
	case "show":
		raw, err := os.ReadFile(file)
		if err != nil {
			fail(err)
		}
		if seed, err = hex.DecodeString(strings.TrimSpace(string(raw))); err != nil || len(seed) != ed25519.SeedSize {
			fail(fmt.Errorf("%s: not a 64-digit hex seed", file))
		}
	default:
		usage()
	}
	pub := ed25519.NewKeyFromSeed(seed).Public().(ed25519.PublicKey)
	fmt.Println(hex.EncodeToString(pub))
}

func fail(err error) {
	fmt.Fprintln(os.Stderr, "witnesskeys:", err)
	os.Exit(1)
}
