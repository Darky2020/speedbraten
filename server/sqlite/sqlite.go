// Package sqlite is a small binding to the SQLite amalgamation in src/sqlite: enough to read
// and update the witness's database (text in, text out).
package sqlite

/*
#cgo CFLAGS: -I${SRCDIR}/../../src/sqlite -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=1 -DSQLITE_ENABLE_DBSTAT_VTAB
#cgo linux LDFLAGS: -lm -ldl -lpthread
#include <stdlib.h>
#include "sqlite3.h"

static int bindtext(sqlite3_stmt *st, int i, const char *s, int n) { return sqlite3_bind_text(st, i, s, n, SQLITE_TRANSIENT); }
*/
import "C"

import (
	"errors"
	"fmt"
	"sync"
	"unsafe"
)

type DB struct {
	mu sync.Mutex
	db *C.sqlite3
}

// Open opens an existing database; it waits up to 5 s for a lock the witness holds.
func Open(path string) (*DB, error) { return open(path, C.SQLITE_OPEN_READWRITE) }

// OpenReadOnly opens an existing database another program writes, only to read it.
func OpenReadOnly(path string) (*DB, error) { return open(path, C.SQLITE_OPEN_READONLY) }

// Create opens a database, making the file if there is none.
func Create(path string) (*DB, error) { return open(path, C.SQLITE_OPEN_READWRITE|C.SQLITE_OPEN_CREATE) }

func open(path string, flags C.int) (*DB, error) {
	cpath := C.CString(path)
	defer C.free(unsafe.Pointer(cpath))
	var db *C.sqlite3
	if rc := C.sqlite3_open_v2(cpath, &db, flags, nil); rc != C.SQLITE_OK {
		if db != nil {
			C.sqlite3_close(db)
		}
		return nil, fmt.Errorf("sqlite: open %s: code %d", path, int(rc))
	}
	C.sqlite3_busy_timeout(db, 5000)
	return &DB{db: db}, nil
}

func (d *DB) Close() {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.db != nil {
		C.sqlite3_close(d.db)
		d.db = nil
	}
}

func (d *DB) err() error { return errors.New("sqlite: " + C.GoString(C.sqlite3_errmsg(d.db))) }

func (d *DB) prepare(query string, args []any) (*C.sqlite3_stmt, error) {
	cq := C.CString(query)
	defer C.free(unsafe.Pointer(cq))
	var st *C.sqlite3_stmt
	if C.sqlite3_prepare_v2(d.db, cq, -1, &st, nil) != C.SQLITE_OK {
		return nil, d.err()
	}
	for i, a := range args {
		var rc C.int
		switch v := a.(type) {
		case int:
			rc = C.sqlite3_bind_int64(st, C.int(i+1), C.sqlite3_int64(v))
		case int64:
			rc = C.sqlite3_bind_int64(st, C.int(i+1), C.sqlite3_int64(v))
		case string:
			cs := C.CString(v)
			rc = C.bindtext(st, C.int(i+1), cs, C.int(len(v)))
			C.free(unsafe.Pointer(cs))
		case nil:
			rc = C.sqlite3_bind_null(st, C.int(i+1))
		default:
			C.sqlite3_finalize(st)
			return nil, fmt.Errorf("sqlite: unsupported argument %T", a)
		}
		if rc != C.SQLITE_OK {
			C.sqlite3_finalize(st)
			return nil, d.err()
		}
	}
	return st, nil
}

// Query returns every row as text; NULL is "".
func (d *DB) Query(query string, args ...any) ([][]string, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	st, err := d.prepare(query, args)
	if err != nil {
		return nil, err
	}
	defer C.sqlite3_finalize(st)
	var rows [][]string
	n := int(C.sqlite3_column_count(st))
	for {
		rc := C.sqlite3_step(st)
		if rc == C.SQLITE_DONE {
			return rows, nil
		}
		if rc != C.SQLITE_ROW {
			return nil, d.err()
		}
		row := make([]string, n)
		for i := 0; i < n; i++ {
			if t := C.sqlite3_column_text(st, C.int(i)); t != nil {
				row[i] = C.GoStringN((*C.char)(unsafe.Pointer(t)), C.sqlite3_column_bytes(st, C.int(i)))
			}
		}
		rows = append(rows, row)
	}
}

// Exec runs a statement that returns no rows and reports how many it changed.
func (d *DB) Exec(query string, args ...any) (int, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	st, err := d.prepare(query, args)
	if err != nil {
		return 0, err
	}
	defer C.sqlite3_finalize(st)
	if rc := C.sqlite3_step(st); rc != C.SQLITE_DONE && rc != C.SQLITE_ROW {
		return 0, d.err()
	}
	return int(C.sqlite3_changes(d.db)), nil
}
