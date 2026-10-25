package main

import (
	"fmt"
	"html/template"
	"strings"
	"time"

	"speedbraten/server/board"
)

// recordGraph draws a board's record over time as a step line in SVG: each record holds until
// the next one, and the last one until now.
func recordGraph(history []board.Record) template.HTML {
	if len(history) == 0 {
		return ""
	}
	const w, h, left, right, top, bottom = 420.0, 210.0, 70.0, 10.0, 12.0, 30.0
	start, end := history[0].When, time.Now()
	if !end.After(start) {
		end = start.Add(time.Hour)
	}
	hi, lo := history[0].MS, history[len(history)-1].MS
	if hi == lo {
		hi, lo = hi+500, lo-500
	}
	pad := (hi - lo) / 10
	hi, lo = hi+pad, lo-pad
	x := func(t time.Time) float64 {
		return left + (w-left-right)*float64(t.Sub(start))/float64(end.Sub(start))
	}
	y := func(ms int64) float64 { return top + (h-top-bottom)*float64(hi-ms)/float64(hi-lo) }

	var b strings.Builder
	fmt.Fprintf(&b, `<svg class="graph" viewBox="0 0 %.0f %.0f" role="img" aria-label="record over time">`, w, h)
	for i := 0; i <= 3; i++ {
		ms := lo + (hi-lo)*int64(i)/3
		fmt.Fprintf(&b, `<line x1="%.0f" x2="%.0f" y1="%.1f" y2="%.1f" stroke="#e5e7eb"/>`, left, w-right, y(ms), y(ms))
		fmt.Fprintf(&b, `<text x="%.0f" y="%.1f" font-size="13" fill="#6b7280" text-anchor="end" dominant-baseline="middle">%s</text>`, left-6, y(ms), board.Time(ms))
	}
	fmt.Fprintf(&b, `<text x="%.0f" y="%.0f" font-size="13" fill="#6b7280">%s</text>`, left, h-8, start.UTC().Format("2 Jan 2006"))
	fmt.Fprintf(&b, `<text x="%.0f" y="%.0f" font-size="13" fill="#6b7280" text-anchor="end">now</text>`, w-right, h-8)
	var path strings.Builder
	for i, r := range history {
		px, py := x(r.When), y(r.MS)
		if i == 0 {
			fmt.Fprintf(&path, "M%.1f %.1f", px, py)
		} else {
			fmt.Fprintf(&path, " H%.1f V%.1f", px, py)
		}
	}
	fmt.Fprintf(&path, " H%.1f", x(end))
	fmt.Fprintf(&b, `<path d="%s" fill="none" stroke="#2563eb" stroke-width="2"/>`, path.String())
	// each point's label shows while the pointer is over the point (CSS only); it sits right of
	// the point and above it, or flips to stay inside the graph
	b.WriteString(`<style>.pt .tip{opacity:0;pointer-events:none;transition:opacity .1s}.pt:hover .tip{opacity:1}.pt:hover .dot{r:5}</style>`)
	const tipw, tiph = 150.0, 40.0
	for _, r := range history {
		px, py := x(r.When), y(r.MS)
		tx, ty := px+10, py-tiph-8
		if tx+tipw > w-right {
			tx = px - 10 - tipw
		}
		if ty < 0 {
			ty = py + 10
		}
		fmt.Fprintf(&b, `<g class="pt"><circle cx="%.1f" cy="%.1f" r="12" fill="transparent"/><circle class="dot" cx="%.1f" cy="%.1f" r="3.5" fill="#2563eb"/>`, px, py, px, py)
		fmt.Fprintf(&b, `<g class="tip"><rect x="%.1f" y="%.1f" width="%.0f" height="%.0f" rx="5" fill="#1f2937"/>`, tx, ty, tipw, tiph)
		fmt.Fprintf(&b, `<text x="%.1f" y="%.1f" font-size="13" font-weight="600" fill="#fff">%s %s</text>`, tx+10, ty+17, board.Time(r.MS), template.HTMLEscapeString(r.Name))
		fmt.Fprintf(&b, `<text x="%.1f" y="%.1f" font-size="12" fill="#d1d5db">%s</text></g></g>`, tx+10, ty+32, r.When.UTC().Format("2 Jan 2006"))
	}
	b.WriteString(`</svg>`)
	return template.HTML(b.String())
}
