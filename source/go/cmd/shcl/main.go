// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier

// shcl CLI - the Go binding's command surface. Flags, output, and exit codes
// mirror the Rust reference exactly; the cicd cross-binding check compares the
// two byte for byte, so any drift here fails the pipeline.
package main

import (
	"fmt"
	"io"
	"os"
	"strconv"
	"strings"
	"unicode/utf8"

	shcl "github.com/jim-collier/shcl/source/go"
)

// Keep in step with source/rust/Cargo.toml, the canonical version source.
const version = "1.0.0-beta1"

const help = `shcl - Simple Hierarchical Config Language (reference CLI)

Usage:
  shcl get [type] [options] FILE PATH    read one value (or array) at a path
  shcl set [options] FILE                apply write-ops (stdin) and print canonical
  shcl fmt [--write|-w] FILE             print (or rewrite in place) the canonical form
  shcl check [options] FILE              load and print diagnostics
  shcl count [options] FILE PATH         number of instances at a path
  shcl instances [options] FILE PATH     instance values at a path, one per line
  shcl help | version                    this help, or the version (also -h/--help, -V/--version)

set reads a write-ops script from stdin (one op per line, tab-separated) and
prints the canonical document. FILE is the base ('-' = empty base). Ops:
  int|float|bool|string|datetime<TAB>PATH<TAB>VALUE       set a scalar
  <type>-array<TAB>PATH<TAB>V1<TAB>V2...                  set an inline array
  <type>[-array]-default<TAB>...                          set only if absent
  raw<TAB>PATH<TAB>INFO<TAB>CONTENT                       set a raw block
  empty<TAB>PATH   comment<TAB>PATH<TAB>TEXT   remove<TAB>PATH
string/raw values decode \n \t \\; a line starting with # is a script comment.

Types (default --string):
  --int --float --bool --datetime --string --raw
  --array                                read the value as an array of the type

Options:
  --default=VALUE                        value to print when the read is not Good
                                         (implies --on-bad=default; for arrays,
                                         substituted per bad slot)
  --on-bad=error|default|flag            error: fail loudly; default: print the
                                         default; flag: print the value anyway and
                                         report via exit code (the default mode)
  --slots                                prefix each line with its slot status and
                                         a tab (per element, or per wildcard slot)
  --strictness=loose|standard|strict     or 1|2|3 (default standard)

Value options accept either spelling: --default=VALUE or --default VALUE.
FILE may be '-' for stdin.

Exit codes: 0 good, 1 usage or I/O error, 2 empty, 3 not found, 4 bad type,
5 multiple instances, 6 strict load failure.
`

func statusCode(st shcl.Status) int {
	switch st {
	case shcl.Good:
		return 0
	case shcl.Empty:
		return 2
	case shcl.NotFound:
		return 3
	case shcl.BadType:
		return 4
	case shcl.Multiple:
		return 5
	}
	return 0
}

type opts struct {
	kind       string // int|float|bool|datetime|string|raw
	array      bool
	slots      bool
	def        string
	onBad      string // error|default|flag
	strictness shcl.Strictness
	write      bool
	args       []string // positional: FILE [PATH]
}

func setValueOpt(o *opts, name, v string) error {
	switch name {
	case "--default":
		o.def = v
		o.onBad = "default"
	case "--on-bad":
		if v != "error" && v != "default" && v != "flag" {
			return fmt.Errorf("bad --on-bad value: %s", v)
		}
		o.onBad = v
	case "--strictness":
		s, ok := shcl.StrictnessFromArg(v)
		if !ok {
			return fmt.Errorf("bad --strictness value: %s", v)
		}
		o.strictness = s
	}
	return nil
}

func parseOpts(argv []string) (*opts, error) {
	o := &opts{kind: "string", onBad: "flag", strictness: shcl.Standard}
	// Value-taking options accept both --opt=VALUE and the space form --opt VALUE.
	for i := 0; i < len(argv); i++ {
		a := argv[i]
		switch {
		case a == "--int" || a == "--float" || a == "--bool" || a == "--datetime" || a == "--string" || a == "--raw":
			o.kind = a[2:]
		case a == "--array":
			o.array = true
		case a == "--slots":
			o.slots = true
		case a == "--write" || a == "-w":
			o.write = true
		case a == "--default" || a == "--on-bad" || a == "--strictness":
			i++
			if i >= len(argv) {
				return nil, fmt.Errorf("missing value for %s (try %s=VALUE)", a, a)
			}
			if err := setValueOpt(o, a, argv[i]); err != nil {
				return nil, err
			}
		case strings.HasPrefix(a, "--default="):
			if err := setValueOpt(o, "--default", a[len("--default="):]); err != nil {
				return nil, err
			}
		case strings.HasPrefix(a, "--on-bad="):
			if err := setValueOpt(o, "--on-bad", a[len("--on-bad="):]); err != nil {
				return nil, err
			}
		case strings.HasPrefix(a, "--strictness="):
			if err := setValueOpt(o, "--strictness", a[len("--strictness="):]); err != nil {
				return nil, err
			}
		case strings.HasPrefix(a, "-") && len(a) > 1:
			return nil, fmt.Errorf("unknown option: %s", a)
		default:
			o.args = append(o.args, a)
		}
	}
	return o, nil
}

func readInput(file string) (string, error) {
	var b []byte
	var err error
	if file == "-" {
		b, err = io.ReadAll(os.Stdin)
		if err != nil {
			return "", fmt.Errorf("stdin: %s", err)
		}
	} else {
		b, err = os.ReadFile(file)
		if err != nil {
			return "", fmt.Errorf("%s: %s", file, err)
		}
	}
	// The reference reads as UTF-8 and fails on bad bytes; match its exit path.
	if !utf8.Valid(b) {
		return "", fmt.Errorf("%s: stream did not contain valid UTF-8", file)
	}
	return string(b), nil
}

func loadDoc(text string, strictness shcl.Strictness) (*shcl.Document, int) {
	doc, err := shcl.ParseWith(text, strictness)
	if err != nil {
		le := err.(*shcl.LoadError)
		for _, d := range le.Diagnostics {
			fmt.Fprintf(os.Stderr, "line %d: %s: %s\n", d.Line, d.Severity, d.Message)
		}
		fmt.Fprintln(os.Stderr, le.Error())
		return nil, 6
	}
	return doc, 0
}

// doGet: one value read, formatted for the shell: scalars print as one line,
// arrays one element per line.
func doGet(o *opts) int {
	if len(o.args) != 2 {
		fmt.Fprintln(os.Stderr, "get needs FILE and PATH (see --help)")
		return 1
	}
	file, path := o.args[0], o.args[1]
	text, err := readInput(file)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	doc, code := loadDoc(text, o.strictness)
	if doc == nil {
		return code
	}
	var lines []string
	var status shcl.Status
	var slots []shcl.Status
	if o.array {
		switch o.kind {
		case "int":
			r := doc.ReadIntArray(path)
			for _, v := range r.Value {
				lines = append(lines, fmt.Sprintf("%d", v))
			}
			status = r.Status
			slots = r.Slots
		case "float":
			r := doc.ReadFloatArray(path)
			for _, v := range r.Value {
				lines = append(lines, shcl.FormatFloat(v))
			}
			status = r.Status
			slots = r.Slots
		case "bool":
			r := doc.ReadBoolArray(path)
			for _, v := range r.Value {
				lines = append(lines, fmt.Sprintf("%t", v))
			}
			status = r.Status
			slots = r.Slots
		case "datetime":
			r := doc.ReadDateTimeArray(path)
			for _, v := range r.Value {
				lines = append(lines, v.String())
			}
			status = r.Status
			slots = r.Slots
		case "raw":
			fmt.Fprintln(os.Stderr, "--raw has no --array form")
			return 1
		default:
			r := doc.ReadStringArray(path)
			lines = r.Value
			status = r.Status
			slots = r.Slots
		}
	} else {
		switch o.kind {
		case "int":
			r := doc.ReadInt(path)
			lines = []string{fmt.Sprintf("%d", r.Value)}
			status = r.Status
		case "float":
			r := doc.ReadFloat(path)
			lines = []string{shcl.FormatFloat(r.Value)}
			status = r.Status
		case "bool":
			r := doc.ReadBool(path)
			lines = []string{fmt.Sprintf("%t", r.Value)}
			status = r.Status
		case "datetime":
			r := doc.ReadDateTime(path)
			lines = []string{r.Value.String()}
			status = r.Status
		case "raw":
			r := doc.ReadRaw(path)
			lines = []string{r.Value}
			status = r.Status
		default:
			r := doc.ReadString(path)
			lines = []string{r.Value}
			status = r.Status
		}
	}
	// Per-line slot status: falls back to the aggregate for scalar reads.
	slotAt := func(i int) shcl.Status {
		if i < len(slots) {
			return slots[i]
		}
		return status
	}
	emit := func(lines []string) {
		for i, l := range lines {
			if o.slots {
				fmt.Printf("%s\t%s\n", slotAt(i), l)
			} else {
				fmt.Println(l)
			}
		}
	}
	switch {
	case status == shcl.Good || (status == shcl.Empty && o.onBad == "flag"):
		emit(lines)
		return statusCode(status)
	case o.onBad == "default":
		if len(slots) > 0 {
			// Array read: the default substitutes per bad slot; alignment holds.
			subbed := make([]string, len(lines))
			for i, l := range lines {
				if slotAt(i) == shcl.Good {
					subbed[i] = l
				} else {
					subbed[i] = o.def
				}
			}
			emit(subbed)
		} else if o.slots {
			fmt.Printf("%s\t%s\n", status, o.def)
		} else {
			fmt.Println(o.def)
		}
		return 0
	case o.onBad == "error":
		typeName := o.kind
		if o.array {
			typeName = o.kind + " array"
		}
		var reason string
		switch status {
		case shcl.BadType:
			if raw := doc.ReadString(path).Raw; raw != nil {
				reason = fmt.Sprintf("value %q is not a valid %s", *raw, typeName)
			} else {
				reason = fmt.Sprintf("value is not a valid %s", typeName)
			}
		case shcl.NotFound:
			reason = "no value at that path"
		case shcl.Empty:
			reason = "the value is empty"
		case shcl.Multiple:
			reason = "the path matches multiple instances"
		}
		fmt.Fprintf(os.Stderr, "shcl: cannot read %s as %s: %s (in %s)\n", path, typeName, reason, file)
		return statusCode(status)
	default:
		// flag: print the zero/empty value anyway; the exit code carries the status
		emit(lines)
		return statusCode(status)
	}
}

func doFmt(o *opts) int {
	if len(o.args) != 1 {
		fmt.Fprintln(os.Stderr, "fmt needs FILE (see --help)")
		return 1
	}
	file := o.args[0]
	if o.write && file == "-" {
		fmt.Fprintln(os.Stderr, "fmt --write cannot rewrite stdin; drop --write to print, or pass a FILE")
		return 1
	}
	text, err := readInput(file)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	doc, code := loadDoc(text, o.strictness)
	if doc == nil {
		return code
	}
	canonical := doc.ToCanonical()
	if o.write {
		if werr := os.WriteFile(file, []byte(canonical), 0o644); werr != nil {
			fmt.Fprintf(os.Stderr, "%s: %s\n", file, werr)
			return 1
		}
	} else {
		fmt.Print(canonical)
	}
	return 0
}

// unescapeOps decodes an ops value: \n \t \\ only; other `\x` stays verbatim.
func unescapeOps(s string) string {
	var b strings.Builder
	for i := 0; i < len(s); i++ {
		if s[i] != '\\' || i+1 >= len(s) {
			b.WriteByte(s[i])
			continue
		}
		i++
		switch s[i] {
		case 'n':
			b.WriteByte('\n')
		case 't':
			b.WriteByte('\t')
		case '\\':
			b.WriteByte('\\')
		default:
			b.WriteByte('\\')
			b.WriteByte(s[i])
		}
	}
	return b.String()
}

func applyOp(doc *shcl.Document, line string) error {
	f := strings.Split(line, "\t")
	get := func(i int) string {
		if i < len(f) {
			return f[i]
		}
		return ""
	}
	path, v := get(1), get(2)
	var arr []string
	if len(f) > 2 {
		arr = f[2:]
	}
	pint := func(s string) (int64, error) { return strconv.ParseInt(s, 10, 64) }
	pflt := func(s string) (float64, error) { return strconv.ParseFloat(s, 64) }
	ints := func(xs []string) ([]int64, error) {
		out := make([]int64, len(xs))
		for i, s := range xs {
			n, err := pint(s)
			if err != nil {
				return nil, err
			}
			out[i] = n
		}
		return out, nil
	}
	flts := func(xs []string) ([]float64, error) {
		out := make([]float64, len(xs))
		for i, s := range xs {
			n, err := pflt(s)
			if err != nil {
				return nil, err
			}
			out[i] = n
		}
		return out, nil
	}
	bools := func(xs []string) []bool {
		out := make([]bool, len(xs))
		for i, s := range xs {
			out[i] = s == "true"
		}
		return out
	}
	strs := func(xs []string) []string {
		out := make([]string, len(xs))
		for i, s := range xs {
			out[i] = unescapeOps(s)
		}
		return out
	}
	dt := func(s string) (shcl.DateTime, error) {
		x, ok := shcl.ParseDateTime(s)
		if !ok {
			return x, fmt.Errorf("bad datetime: %s", s)
		}
		return x, nil
	}
	dts := func(xs []string) ([]shcl.DateTime, error) {
		out := make([]shcl.DateTime, len(xs))
		for i, s := range xs {
			x, err := dt(s)
			if err != nil {
				return nil, err
			}
			out[i] = x
		}
		return out, nil
	}
	switch f[0] {
	case "int":
		n, err := pint(v)
		if err != nil {
			return err
		}
		doc.SetInt(path, n)
	case "float":
		n, err := pflt(v)
		if err != nil {
			return err
		}
		doc.SetFloat(path, n)
	case "bool":
		doc.SetBool(path, v == "true")
	case "string":
		doc.SetString(path, unescapeOps(v))
	case "datetime":
		x, err := dt(v)
		if err != nil {
			return err
		}
		doc.SetDateTime(path, x)
	case "int-default":
		n, err := pint(v)
		if err != nil {
			return err
		}
		doc.SetIntDefault(path, n)
	case "float-default":
		n, err := pflt(v)
		if err != nil {
			return err
		}
		doc.SetFloatDefault(path, n)
	case "bool-default":
		doc.SetBoolDefault(path, v == "true")
	case "string-default":
		doc.SetStringDefault(path, unescapeOps(v))
	case "datetime-default":
		x, err := dt(v)
		if err != nil {
			return err
		}
		doc.SetDateTimeDefault(path, x)
	case "int-array":
		xs, err := ints(arr)
		if err != nil {
			return err
		}
		doc.SetIntArray(path, xs)
	case "float-array":
		xs, err := flts(arr)
		if err != nil {
			return err
		}
		doc.SetFloatArray(path, xs)
	case "bool-array":
		doc.SetBoolArray(path, bools(arr))
	case "string-array":
		doc.SetStringArray(path, strs(arr))
	case "datetime-array":
		xs, err := dts(arr)
		if err != nil {
			return err
		}
		doc.SetDateTimeArray(path, xs)
	case "int-array-default":
		xs, err := ints(arr)
		if err != nil {
			return err
		}
		doc.SetIntArrayDefault(path, xs)
	case "float-array-default":
		xs, err := flts(arr)
		if err != nil {
			return err
		}
		doc.SetFloatArrayDefault(path, xs)
	case "bool-array-default":
		doc.SetBoolArrayDefault(path, bools(arr))
	case "string-array-default":
		doc.SetStringArrayDefault(path, strs(arr))
	case "datetime-array-default":
		xs, err := dts(arr)
		if err != nil {
			return err
		}
		doc.SetDateTimeArrayDefault(path, xs)
	case "raw":
		doc.SetRaw(path, unescapeOps(get(3)), v)
	case "raw-default":
		doc.SetRawDefault(path, unescapeOps(get(3)), v)
	case "empty":
		doc.SetEmpty(path)
	case "comment":
		doc.SetComment(path, v)
	case "remove":
		doc.Remove(path)
	default:
		return fmt.Errorf("unknown op: %s", f[0])
	}
	return nil
}

func doSet(o *opts) int {
	if len(o.args) != 1 {
		fmt.Fprintln(os.Stderr, "set needs FILE (ops on stdin; see --help)")
		return 1
	}
	file := o.args[0]
	// Base doc: '-' means an empty base, since stdin carries the ops script.
	text := ""
	if file != "-" {
		t, err := readInput(file)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 1
		}
		text = t
	}
	doc, code := loadDoc(text, o.strictness)
	if doc == nil {
		return code
	}
	ops, err := io.ReadAll(os.Stdin)
	if err != nil {
		fmt.Fprintf(os.Stderr, "stdin: %s\n", err)
		return 1
	}
	for n, line := range strings.Split(string(ops), "\n") {
		line = strings.TrimSuffix(line, "\r") // match Rust lines() CRLF handling
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		if aerr := applyOp(doc, line); aerr != nil {
			fmt.Fprintf(os.Stderr, "op line %d: %s\n", n+1, aerr)
			return 1
		}
	}
	fmt.Print(doc.ToCanonical())
	return 0
}

func doCheck(o *opts) int {
	if len(o.args) != 1 {
		fmt.Fprintln(os.Stderr, "check needs FILE (see --help)")
		return 1
	}
	text, err := readInput(o.args[0])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	doc, perr := shcl.ParseWith(text, o.strictness)
	if perr != nil {
		le := perr.(*shcl.LoadError)
		for _, d := range le.Diagnostics {
			fmt.Printf("line %d: %s: %s\n", d.Line, d.Severity, d.Message)
		}
		fmt.Println(le.Error())
		return 6
	}
	for _, d := range doc.Diagnostics() {
		fmt.Printf("line %d: %s: %s\n", d.Line, d.Severity, d.Message)
	}
	fmt.Printf("ok (%d diagnostic(s))\n", len(doc.Diagnostics()))
	return 0
}

func doEnum(o *opts, wantCount bool) int {
	if len(o.args) != 2 {
		fmt.Fprintln(os.Stderr, "count/instances need FILE and PATH (see --help)")
		return 1
	}
	file, path := o.args[0], o.args[1]
	text, err := readInput(file)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	doc, code := loadDoc(text, o.strictness)
	if doc == nil {
		return code
	}
	if wantCount {
		fmt.Println(doc.Count(path))
	} else {
		for _, v := range doc.Instances(path) {
			fmt.Println(v)
		}
	}
	return 0
}

func run() int {
	argv := os.Args[1:]
	for _, a := range argv {
		if !utf8.ValidString(a) {
			fmt.Fprintln(os.Stderr, "invalid argument encoding (expected UTF-8)")
			return 1
		}
	}
	wantsHelp := false
	wantsVersion := false
	for _, a := range argv {
		if a == "-h" || a == "--help" {
			wantsHelp = true
		}
		if a == "-V" || a == "--version" {
			wantsVersion = true
		}
	}
	if len(argv) == 0 || wantsHelp || argv[0] == "help" {
		fmt.Print(help)
		if len(argv) == 0 {
			return 1
		}
		return 0
	}
	if wantsVersion || argv[0] == "version" {
		fmt.Printf("shcl %s\n", version)
		return 0
	}
	o, err := parseOpts(argv[1:])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	switch argv[0] {
	case "get":
		return doGet(o)
	case "set":
		return doSet(o)
	case "fmt":
		return doFmt(o)
	case "check":
		return doCheck(o)
	case "count":
		return doEnum(o, true)
	case "instances":
		return doEnum(o, false)
	default:
		fmt.Fprintf(os.Stderr, "unknown command: %s (see --help)\n", argv[0])
		return 1
	}
}

func main() {
	os.Exit(run())
}
