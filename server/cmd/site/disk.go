package main

import (
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"speedbraten/server/sqlite"
)

// diskReport is the server's disk and the space speedbraten's folders take on it.
type diskReport struct {
	Total, Used, Free uint64
	UsedPercent       int
	Folders           []folderSize
	Ours              uint64
	Databases         []dbSize
	At                time.Time
}

type folderSize struct {
	Name, Path string
	Size       uint64
	Files      int
	Err        string
	Entries    []entrySize // what's inside, two levels deep, biggest first
}

// entrySize is a subfolder, a file, or the files of one extension that share a folder ("*.json").
type entrySize struct {
	Name    string
	Dir     bool
	Size    uint64
	Files   int
	Entries []entrySize
}

// dbSize is a SQLite database found in the folders, by table and index.
type dbSize struct {
	Path      string
	Size, WAL uint64
	Objects   []dbObject
	Err       string
}

type dbObject struct {
	Name, Type string
	Rows       int64 // -1 for an index
	Size       uint64
}

const (
	topEntries   = 8 // per folder
	topNested    = 5 // inside a subfolder
	nestedShare  = 5 // percent of the folder a subfolder needs to be broken down
	groupFiles   = 3 // files of one extension in a folder beyond which they show as one "*.ext" line
	topDBObjects = 8
)

// diskWatch measures the folders at most every ttl; walking a folder of many replays takes a while.
type diskWatch struct {
	folders []folderSize // name and path only
	ttl     time.Duration

	mu   sync.Mutex
	last *diskReport
}

// parseFolders reads "name=path,name=path".
func parseFolders(spec string) ([]folderSize, error) {
	var out []folderSize
	for _, part := range strings.Split(spec, ",") {
		part = strings.TrimSpace(part)
		if part == "" {
			continue
		}
		name, path, ok := strings.Cut(part, "=")
		if !ok || name == "" || path == "" {
			return nil, fmt.Errorf("-disk: %q isn't name=path", part)
		}
		out = append(out, folderSize{Name: name, Path: path})
	}
	return out, nil
}

func (d *diskWatch) report() *diskReport {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.last != nil && time.Since(d.last.At) < d.ttl {
		return d.last
	}
	r := &diskReport{At: time.Now()}
	if len(d.folders) > 0 {
		var st syscall.Statfs_t
		if syscall.Statfs(d.folders[0].Path, &st) == nil {
			r.Total = st.Blocks * uint64(st.Bsize)
			r.Free = st.Bavail * uint64(st.Bsize)
			r.Used = r.Total - st.Bfree*uint64(st.Bsize)
			if r.Total > 0 {
				r.UsedPercent = int(r.Used * 100 / r.Total)
			}
		}
	}
	for _, f := range d.folders {
		f.Size, f.Files, f.Err, f.Entries = 0, 0, "", nil
		tree := &node{children: map[string]*node{}}
		var dbs []string
		err := filepath.WalkDir(f.Path, func(path string, e fs.DirEntry, err error) error {
			if err != nil || !e.Type().IsRegular() {
				return nil
			}
			info, err := e.Info()
			if err != nil {
				return nil
			}
			size := uint64(info.Size())
			f.Size += size
			f.Files++
			rel, _ := filepath.Rel(f.Path, path)
			parts := strings.Split(filepath.ToSlash(rel), "/")
			tree.add(parts, size)
			if strings.HasSuffix(path, ".db") && len(parts) <= 2 {
				dbs = append(dbs, path)
			}
			return nil
		})
		if err != nil {
			f.Err = err.Error()
		}
		f.Entries = tree.entries(topEntries, f.Size, 0)
		r.Ours += f.Size
		r.Folders = append(r.Folders, f)
		for _, p := range dbs {
			r.Databases = append(r.Databases, measureDB(p))
		}
	}
	d.last = r
	return r
}

// node is a folder of the walk: its subfolders by name, and its own files by extension.
type node struct {
	children map[string]*node
	files    map[string][]fileSize // extension -> files
	size     uint64
	count    int
}

type fileSize struct {
	name string
	size uint64
}

func (n *node) add(parts []string, size uint64) {
	n.size += size
	n.count++
	if len(parts) == 1 {
		if n.files == nil {
			n.files = map[string][]fileSize{}
		}
		ext := filepath.Ext(parts[0])
		n.files[ext] = append(n.files[ext], fileSize{parts[0], size})
		return
	}
	c := n.children[parts[0]]
	if c == nil {
		c = &node{children: map[string]*node{}}
		n.children[parts[0]] = c
	}
	c.add(parts[1:], size)
}

// entries lists a folder's subfolders and files, biggest first; subfolders holding at least nestedShare percent of
// the measured folder are broken down one level further.
func (n *node) entries(top int, total uint64, depth int) []entrySize {
	var out []entrySize
	for name, c := range n.children {
		e := entrySize{Name: name + "/", Dir: true, Size: c.size, Files: c.count}
		if depth == 0 && total > 0 && c.size*100 >= total*nestedShare {
			e.Entries = c.entries(topNested, total, depth+1)
		}
		out = append(out, e)
	}
	for ext, files := range n.files {
		if len(files) > groupFiles && ext != "" {
			e := entrySize{Name: "*" + ext, Files: len(files)}
			for _, f := range files {
				e.Size += f.size
			}
			out = append(out, e)
			continue
		}
		for _, f := range files {
			out = append(out, entrySize{Name: f.name, Size: f.size, Files: 1})
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Size > out[j].Size })
	if len(out) > top {
		rest := entrySize{Name: fmt.Sprintf("%d more", len(out)-top)}
		for _, e := range out[top:] {
			rest.Size += e.Size
			rest.Files += e.Files
		}
		out = append(out[:top], rest)
	}
	return out
}

// measureDB reads a database's tables and indexes from dbstat, read-only.
func measureDB(path string) dbSize {
	r := dbSize{Path: path}
	if info, err := os.Stat(path); err == nil {
		r.Size = uint64(info.Size())
	}
	if info, err := os.Stat(path + "-wal"); err == nil {
		r.WAL = uint64(info.Size())
	}
	db, err := sqlite.OpenReadOnly(path)
	if err != nil {
		r.Err = err.Error()
		return r
	}
	defer db.Close()
	rows, err := db.Query("SELECT s.name, IFNULL(m.type, 'table'), SUM(s.pgsize) FROM dbstat s LEFT JOIN sqlite_master m ON m.name = s.name GROUP BY s.name ORDER BY 3 DESC")
	if err != nil {
		r.Err = err.Error()
		return r
	}
	for i, row := range rows {
		size, _ := strconv.ParseUint(row[2], 10, 64)
		o := dbObject{Name: row[0], Type: row[1], Rows: -1, Size: size}
		if i < topDBObjects && o.Type == "table" && !strings.HasPrefix(o.Name, "sqlite_") {
			if n, err := db.Query("SELECT COUNT(*) FROM \"" + strings.ReplaceAll(o.Name, "\"", "\"\"") + "\""); err == nil && len(n) == 1 {
				o.Rows, _ = strconv.ParseInt(n[0][0], 10, 64)
			}
		}
		if i == topDBObjects {
			rest := dbObject{Name: fmt.Sprintf("%d more", len(rows)-topDBObjects), Rows: -1}
			for _, more := range rows[topDBObjects:] {
				n, _ := strconv.ParseUint(more[2], 10, 64)
				rest.Size += n
			}
			r.Objects = append(r.Objects, rest)
			break
		}
		r.Objects = append(r.Objects, o)
	}
	return r
}

func bytesize(n uint64) string {
	switch {
	case n >= 1<<30:
		return fmt.Sprintf("%.1f GB", float64(n)/(1<<30))
	case n >= 1<<20:
		return fmt.Sprintf("%.1f MB", float64(n)/(1<<20))
	case n >= 1<<10:
		return fmt.Sprintf("%.0f KB", float64(n)/(1<<10))
	}
	return fmt.Sprintf("%d B", n)
}
