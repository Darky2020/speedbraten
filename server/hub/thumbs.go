package hub

import (
	"image"
	"image/jpeg"
	"os"
	"path/filepath"
	"sync"
	"time"

	"golang.org/x/image/draw"
	_ "golang.org/x/image/webp"
)

// Thumbnail size and quality: square like the game's map previews, about 12 KB a picture.
const (
	ThumbW       = 256
	ThumbH       = 256
	thumbQuality = 80
	// MaxThumb is the largest picture sent; MsgThumbData carries it whole.
	MaxThumb = 60 << 10
)

type thumbSizes struct {
	mu   sync.Mutex
	size map[string]int
}

func (h *Hub) thumbFile(name string) string { return filepath.Join(h.cfg.Thumbs, name+".jpg") }

// RefreshThumbs makes the pictures of race maps whose screenshot is newer than their picture and
// forgets maps that are gone; progress, when not nil, hears about every picture made. It returns
// how many it made.
func (h *Hub) RefreshThumbs(progress func(done, total int)) int {
	if h.cfg.Thumbs == "" || h.cfg.RaceMaps == "" {
		return 0
	}
	os.MkdirAll(h.cfg.Thumbs, 0o755)
	maps := h.raceMaps()
	shots := filepath.Join(h.cfg.RaceMaps, "packages", "base", "screenshots")
	sizes := make(map[string]int, len(maps))
	type todo struct{ name, src string }
	var work []todo
	for _, m := range maps {
		src := filepath.Join(shots, m+".webp")
		ss, err := os.Stat(src)
		if err != nil || !ss.Mode().IsRegular() {
			continue
		}
		if ts, err := os.Stat(h.thumbFile(m)); err == nil && !ts.ModTime().Before(ss.ModTime()) && ts.Size() > 0 && ts.Size() <= MaxThumb && thumbShaped(h.thumbFile(m)) {
			sizes[m] = int(ts.Size())
			continue
		}
		work = append(work, todo{m, src})
	}
	h.setThumbs(sizes)
	made := 0
	for i, t := range work {
		if n, err := makeThumb(t.src, h.thumbFile(t.name)); err == nil {
			sizes[t.name] = n
			made++
		}
		if progress != nil {
			progress(i+1, len(work))
		}
		if (i+1)%50 == 0 {
			h.setThumbs(sizes)
		}
	}
	h.setThumbs(sizes)
	return made
}

// thumbShaped: whether a picture made before has the current size
func thumbShaped(path string) bool {
	f, err := os.Open(path)
	if err != nil {
		return false
	}
	defer f.Close()
	c, _, err := image.DecodeConfig(f)
	return err == nil && c.Width == ThumbW && c.Height == ThumbH
}

func (h *Hub) setThumbs(sizes map[string]int) {
	c := make(map[string]int, len(sizes))
	for k, v := range sizes {
		c[k] = v
	}
	h.thumbs.mu.Lock()
	h.thumbs.size = c
	h.thumbs.mu.Unlock()
}

// makeThumb writes src scaled to ThumbW x ThumbH (cropped to that shape) as a JPEG; the size written.
func makeThumb(src, dst string) (int, error) {
	f, err := os.Open(src)
	if err != nil {
		return 0, err
	}
	img, _, err := image.Decode(f)
	f.Close()
	if err != nil {
		return 0, err
	}
	b := img.Bounds()
	crop := b
	if b.Dx()*ThumbH > b.Dy()*ThumbW {
		w := b.Dy() * ThumbW / ThumbH
		crop.Min.X += (b.Dx() - w) / 2
		crop.Max.X = crop.Min.X + w
	} else {
		hh := b.Dx() * ThumbH / ThumbW
		crop.Min.Y += (b.Dy() - hh) / 2
		crop.Max.Y = crop.Min.Y + hh
	}
	out := image.NewRGBA(image.Rect(0, 0, ThumbW, ThumbH))
	draw.CatmullRom.Scale(out, out.Bounds(), img, crop, draw.Src, nil)
	tmp := dst + ".part"
	w, err := os.Create(tmp)
	if err != nil {
		return 0, err
	}
	err = jpeg.Encode(w, out, &jpeg.Options{Quality: thumbQuality})
	if cerr := w.Close(); err == nil {
		err = cerr
	}
	st, serr := os.Stat(tmp)
	if err == nil && serr == nil && st.Size() > MaxThumb {
		err = os.ErrInvalid
	}
	if err != nil {
		os.Remove(tmp)
		return 0, err
	}
	if err := os.Rename(tmp, dst); err != nil {
		return 0, err
	}
	return int(st.Size()), nil
}

func thumbDataMsg(name string, data []byte) []byte {
	var w writer
	w.str(name)
	w.bytes(data)
	return w.b
}

// thumbRequest sends a race map's picture, or an empty one when there is none. Called with h.mu held.
func (h *Hub) thumbRequest(p *player, r *reader) error {
	name := r.str(64)
	if err := r.done(); err != nil {
		return err
	}
	h.thumbs.mu.Lock()
	known := h.thumbs.size[name] > 0
	h.thumbs.mu.Unlock()
	var data []byte
	if known && validMapName(name) {
		if b, err := os.ReadFile(h.thumbFile(name)); err == nil && len(b) <= MaxThumb {
			data = b
		}
	}
	h.send(p, MsgThumbData, thumbDataMsg(name, data))
	return nil
}

// ThumbLoop keeps the race map pictures up to date, logging through logf.
func (h *Hub) ThumbLoop(logf func(string, ...any)) {
	for {
		start, last := time.Now(), time.Now()
		made := h.RefreshThumbs(func(done, total int) {
			if time.Since(last) >= 30*time.Second || done == total {
				last = time.Now()
				el := time.Since(start)
				rate := float64(done) / el.Seconds()
				logf("race map pictures: %d/%d (%.0f%%) in %v, %.1f/s, eta %v", done, total, 100*float64(done)/float64(total),
					el.Round(time.Second), rate, time.Duration(float64(total-done)/rate*float64(time.Second)).Round(time.Second))
			}
		})
		if made > 0 {
			logf("race map pictures: made %d in %v", made, time.Since(start).Round(time.Millisecond))
		}
		time.Sleep(time.Minute)
	}
}
