package main

import (
	"fmt"
	"io/fs"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"time"
)

// diskReport is the server's disk and the space speedbraten's folders take on it.
type diskReport struct {
	Total, Used, Free uint64
	UsedPercent       int
	Folders           []folderSize
	Ours              uint64
	At                time.Time
}

type folderSize struct {
	Name, Path string
	Size       uint64
	Files      int
	Err        string
}

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
		f.Size, f.Files, f.Err = 0, 0, ""
		err := filepath.WalkDir(f.Path, func(_ string, e fs.DirEntry, err error) error {
			if err != nil {
				return nil
			}
			if e.Type().IsRegular() {
				if info, err := e.Info(); err == nil {
					f.Size += uint64(info.Size())
					f.Files++
				}
			}
			return nil
		})
		if err != nil {
			f.Err = err.Error()
		}
		r.Ours += f.Size
		r.Folders = append(r.Folders, f)
	}
	d.last = r
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
