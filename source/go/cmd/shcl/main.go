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
	"path/filepath"
	"strconv"
	"strings"
	"unicode/utf8"

	shcl "github.com/jim-collier/shcl/source/go"
)

// Keep in step with source/rust/Cargo.toml, the canonical version source.
const version = "1.0.0"

const help = `shcl - Simple Hierarchical Config Language (reference CLI)

Usage:
  shcl get [type] [options] FILE PATH    read one value (or array) at a path
  shcl set [--write|-w] [options] FILE   apply edits (--set, or ops on stdin);
                                         print canonical (or rewrite FILE in
                                         place with --write)
  shcl fmt [--write|-w] FILE             print (or rewrite in place) the canonical form
  shcl check [options] FILE              load and print diagnostics
                                         (--schema=SCHEMA also validates FILE
                                         against a schema, itself a .shcl file)
  shcl init --schema=SCHEMA              print a commented starter config from
                                         a schema (required fields live, optional
                                         commented, wildcards noted)
  shcl count [options] FILE PATH         number of instances at a path
  shcl instances [options] FILE PATH     instance values at a path, one per line
  shcl help | version                    this help, or the version (also -h/--help, -V/--version)

set edits FILE, the base document ('-' = empty base). Scalars go in as
repeatable --set PATH=VALUE options, which persist with --write; given any
--set, no ops are read from stdin. Everything else - arrays, raw blocks,
set-only-if-absent, removal - goes in as a write-ops script on stdin, one op
per line, tab-separated. Ops:
  int|float|bool|string|datetime<TAB>PATH<TAB>VALUE       set a scalar
  <type>-array<TAB>PATH<TAB>V1<TAB>V2...                  set an inline array
  <type>[-array]-default<TAB>...                          set only if absent
  raw<TAB>PATH<TAB>INFO<TAB>CONTENT                       set a raw block
  empty<TAB>PATH   comment<TAB>PATH<TAB>TEXT   remove<TAB>PATH
string/raw values decode \n \t \\; a line starting with # is a script comment.

Types (default --string):
  --int --float --bool --datetime --string --raw --rawinfo
  --array                                read the value as an array of the type
  --rawinfo reads a raw block's info-string (the fence tag), not its content

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
  --schema=SCHEMA                        (check/init) validate FILE against a
                                         schema; adds V### diagnostics
  --layer=FILE                           (get/fmt/count/instances/set) merge a
                                         lower-priority layer under FILE;
                                         repeatable, earlier = lower priority
  --set=PATH=VALUE                       override one path as the top layer,
                                         after all files; repeatable. On 'set'
                                         it is an edit to the document itself,
                                         so it persists with --write. VALUE is
                                         written as literal config text, so its
                                         type follows the text (8 is an int,
                                         hello is a string)

Value options accept either spelling: --default=VALUE or --default VALUE. In
the space form the next argument is taken as the value whatever it looks like,
so --default --int reads --int as the default. Use -- to end the options when a
FILE or PATH begins with a dash.
An option a subcommand does not use is a usage error, not ignored.
FILE may be '-' for stdin. With --layer, FILE is the highest file layer and
each --layer is merged under it in order; --set applies last. 'fmt' with
layers prints the merged canonical document.

Exit codes: 0 good, 1 usage or I/O error, 2 empty, 3 not found, 4 bad type,
5 multiple instances, 6 check failed or strict load failure.
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
	schema     string
	layers     []string    // lower-priority layers, in listed order
	sets       [][2]string // final override layer: path=value
	args       []string    // positional: FILE [PATH]
	seen       []string    // canonical names of options given, for per-command validation
}

// askedFor: did the command line ask for help or the version? Only tokens in
// option position count: a value that happens to read `-h`, and anything after
// the file, are data. Scanning the whole line for them let a read of a missing
// path answer with the help text and exit 0.
func askedFor(argv []string) string {
	for i := 0; i < len(argv); i++ {
		a := argv[i]
		switch {
		case a == "-h" || a == "--help":
			return "help"
		case a == "-V" || a == "--version":
			return "version"
		case a == "--":
			return ""
		case a == "--default" || a == "--on-bad" || a == "--strictness" || a == "--schema" || a == "--layer" || a == "--set":
			i++
		case strings.HasPrefix(a, "-") && len(a) > 1:
		case i > 0:
			// The subcommand, then the file: past that everything is a path.
			return ""
		}
	}
	return ""
}

func setValueOpt(o *opts, name, v string) error {
	switch name {
	case "--default":
		o.def = v
		o.onBad = "default"
		o.seen = append(o.seen, "--default")
	case "--on-bad":
		if v != "error" && v != "default" && v != "flag" {
			return fmt.Errorf("bad --on-bad value: %s", v)
		}
		o.onBad = v
		o.seen = append(o.seen, "--on-bad")
	case "--strictness":
		s, ok := shcl.StrictnessFromArg(v)
		if !ok {
			return fmt.Errorf("bad --strictness value: %s", v)
		}
		o.strictness = s
		o.seen = append(o.seen, "--strictness")
	case "--schema":
		o.schema = v
		o.seen = append(o.seen, "--schema")
	case "--layer":
		o.layers = append(o.layers, v)
		o.seen = append(o.seen, "--layer")
	case "--set":
		eq := strings.IndexByte(v, '=')
		if eq < 0 {
			return fmt.Errorf("bad --set value (want PATH=VALUE): %s", v)
		}
		o.sets = append(o.sets, [2]string{v[:eq], v[eq+1:]})
		o.seen = append(o.seen, "--set")
	}
	return nil
}

func parseOpts(argv []string) (*opts, error) {
	o := &opts{kind: "string", onBad: "flag", strictness: shcl.Standard}
	// Value-taking options accept both --opt=VALUE and the space form --opt VALUE.
	for i := 0; i < len(argv); i++ {
		a := argv[i]
		// Everything after `--` is positional, so a file or path may begin
		// with a dash.
		if a == "--" {
			o.args = append(o.args, argv[i+1:]...)
			return o, nil
		}
		switch {
		case a == "--int" || a == "--float" || a == "--bool" || a == "--datetime" || a == "--string" || a == "--raw" || a == "--rawinfo":
			o.kind = a[2:]
			o.seen = append(o.seen, "--<type>")
		case a == "--array":
			o.array = true
			o.seen = append(o.seen, "--array")
		case a == "--slots":
			o.slots = true
			o.seen = append(o.seen, "--slots")
		case a == "--write" || a == "-w":
			o.write = true
			o.seen = append(o.seen, "--write")
		case a == "--default" || a == "--on-bad" || a == "--strictness" || a == "--schema" || a == "--layer" || a == "--set":
			i++
			if i >= len(argv) {
				return nil, fmt.Errorf("missing value for %s (try %s=VALUE)", a, a)
			}
			if err := setValueOpt(o, a, argv[i]); err != nil {
				return nil, err
			}
		case strings.HasPrefix(a, "--layer="):
			if err := setValueOpt(o, "--layer", a[len("--layer="):]); err != nil {
				return nil, err
			}
		case strings.HasPrefix(a, "--set="):
			if err := setValueOpt(o, "--set", a[len("--set="):]); err != nil {
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
		case strings.HasPrefix(a, "--schema="):
			if err := setValueOpt(o, "--schema", a[len("--schema="):]); err != nil {
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

// checkOpts: every option must be meaningful for its subcommand; an option that
// would be silently ignored (`set --write` before it existed, `--schema` on
// `get`) is a usage error instead.
func checkOpts(cmd string, o *opts) int {
	var allowed []string
	switch cmd {
	case "get":
		allowed = []string{"--<type>", "--array", "--slots", "--default", "--on-bad", "--strictness", "--layer", "--set"}
	case "set":
		allowed = []string{"--strictness", "--layer", "--set", "--write"}
	case "fmt":
		allowed = []string{"--write", "--strictness", "--layer", "--set"}
	case "check":
		allowed = []string{"--strictness", "--schema"}
	case "init":
		allowed = []string{"--schema"}
	case "count", "instances":
		allowed = []string{"--strictness", "--layer", "--set"}
	}
	for _, s := range o.seen {
		ok := false
		for _, a := range allowed {
			if s == a {
				ok = true
				break
			}
		}
		if !ok {
			if s == "--<type>" {
				fmt.Fprintf(os.Stderr, "type options are not valid for %s (see --help)\n", cmd)
			} else {
				fmt.Fprintf(os.Stderr, "option %s not valid for %s (see --help)\n", s, cmd)
			}
			return 1
		}
	}
	// Writing back the merged document would fold the lower layers permanently
	// into the top file, which is the opposite of what layering is for. On 'set'
	// the --set values are edits to the document rather than a layer over it, so
	// persisting them is the whole point; everywhere else they stay ephemeral.
	if o.write && len(o.layers) > 0 {
		fmt.Fprintln(os.Stderr, "--write cannot be combined with --layer (see --help)")
		return 1
	}
	if o.write && len(o.sets) > 0 && cmd != "set" {
		fmt.Fprintln(os.Stderr, "--write cannot be combined with --set (see --help)")
		return 1
	}
	// The ops script already has stdin, so a layer cannot read it too.
	if cmd == "set" {
		for _, l := range o.layers {
			if l == "-" {
				fmt.Fprintln(os.Stderr, "--layer=- is not valid for set (stdin carries the ops script)")
				return 1
			}
		}
	}
	return 0
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

// loadLayered loads file with o's lower-priority --layer files underneath and
// its --set overrides on top - the layered-load fold. Every layer parses at the
// requested strictness; a strict-load failure on any layer aborts like a
// single-file strict failure (exit 6). Returns (doc, 0) or (nil, code).
func loadLayered(o *opts, file string) (*shcl.Document, int) {
	texts := make([]string, 0, len(o.layers)+1)
	for _, lf := range o.layers {
		t, err := readInput(lf)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			return nil, 1
		}
		texts = append(texts, t)
	}
	base, err := readInput(file)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return nil, 1
	}
	texts = append(texts, base)
	doc, code := loadDoc(texts[0], o.strictness)
	if code != 0 {
		return nil, code
	}
	for _, t := range texts[1:] {
		over, c := loadDoc(t, o.strictness)
		if c != 0 {
			return nil, c
		}
		doc.Merge(over)
	}
	for _, s := range o.sets {
		if !doc.SetString(s[0], s[1]) {
			fmt.Fprintf(os.Stderr, "shcl: cannot write %s (from --set)\n", s[0])
			return nil, 1
		}
	}
	return doc, 0
}

// writeAtomic writes via a temp file in the same dir, then renames over the
// target, so an interrupted write can never truncate the config it rewrites.
// The data is synced before the rename so a crash cannot publish an empty file.
//
// A rename publishes a new inode, so the target is resolved through symlinks
// first (otherwise a linked-in config gets replaced by a regular file and the
// real one is left stale) and the original's mode is copied onto the temp file
// (otherwise a 600 config comes back at whatever the umask allows). Other hard
// links to the old inode cannot survive a rename and keep the old content.
func writeAtomic(file, data string) error {
	// EvalSymlinks fails when the target does not exist yet; that is a plain
	// create, so the path as given is already the right one.
	target := file
	if resolved, rerr := filepath.EvalSymlinks(file); rerr == nil {
		target = resolved
	}
	dir := filepath.Dir(target)
	base := filepath.Base(target)
	// Exclusive create: the name is predictable, so anything already sitting
	// there - including a symlink someone else planted - must make this fail
	// rather than be written through. Retry past a stale collision, then give
	// up; refusing to write beats writing somewhere unintended.
	var f *os.File
	var tmp string
	var last error
	for attempt := 0; attempt < 8; attempt++ {
		tmp = filepath.Join(dir, "."+base+".tmp"+strconv.Itoa(os.Getpid())+"."+strconv.Itoa(attempt))
		// Born private, so the copy is never briefly readable to anyone the
		// original was not. The real mode goes on below, before any data.
		h, oerr := os.OpenFile(tmp, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600)
		if oerr == nil {
			f = h
			break
		}
		last = oerr
	}
	if f == nil {
		return fmt.Errorf("%s: cannot create temporary file: %s", file, last)
	}
	// On the handle, so umask cannot narrow it the way it narrows a create
	// mode. Best effort: a filesystem that cannot carry the mode is not a
	// reason to fail a write that otherwise succeeded.
	if st, serr := os.Stat(target); serr == nil {
		_ = f.Chmod(st.Mode().Perm())
	}
	var err error
	if _, werr := f.WriteString(data); werr != nil {
		err = werr
	} else {
		err = f.Sync()
	}
	f.Close()
	if err != nil {
		os.Remove(tmp)
		return fmt.Errorf("%s: %s", file, err)
	}
	if rerr := os.Rename(tmp, target); rerr != nil {
		os.Remove(tmp)
		return fmt.Errorf("%s: %s", file, rerr)
	}
	return nil
}

// doGet: one value read, formatted for the shell: scalars print as one line,
// arrays one element per line.
func doGet(o *opts) int {
	if len(o.args) != 2 {
		fmt.Fprintln(os.Stderr, "get needs FILE and PATH (see --help)")
		return 1
	}
	file, path := o.args[0], o.args[1]
	doc, code := loadLayered(o, file)
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
		case "raw", "rawinfo":
			fmt.Fprintf(os.Stderr, "--%s has no --array form\n", o.kind)
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
		case "rawinfo":
			r := doc.ReadRawInfo(path)
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
	doc, code := loadLayered(o, file)
	if doc == nil {
		return code
	}
	canonical := doc.ToCanonical()
	if o.write {
		if werr := writeAtomic(file, canonical); werr != nil {
			fmt.Fprintln(os.Stderr, werr)
			return 1
		}
	} else {
		fmt.Print(canonical)
	}
	return 0
}

// intGrammar matches the reference's i64 FromStr: optional single sign, then
// one or more ASCII digits, nothing else.
func intGrammar(s string) bool {
	if s != "" && (s[0] == '+' || s[0] == '-') {
		s = s[1:]
	}
	if s == "" {
		return false
	}
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return false
		}
	}
	return true
}

// floatGrammar matches the reference's f64 FromStr: optional sign, then
// inf|infinity|nan (any case) or ASCII-digit decimal with optional fraction
// and exponent. No hex, no underscores, no whitespace.
func floatGrammar(s string) bool {
	if s != "" && (s[0] == '+' || s[0] == '-') {
		s = s[1:]
	}
	if s == "" {
		return false
	}
	low := strings.ToLower(s)
	if low == "inf" || low == "infinity" || low == "nan" {
		return true
	}
	i := 0
	digits := func() int {
		n := 0
		for i < len(s) && s[i] >= '0' && s[i] <= '9' {
			i++
			n++
		}
		return n
	}
	if digits() > 0 {
		if i < len(s) && s[i] == '.' {
			i++
			digits()
		}
	} else {
		if s[i] != '.' {
			return false
		}
		i++
		if digits() == 0 {
			return false
		}
	}
	if i < len(s) && (s[i] == 'e' || s[i] == 'E') {
		i++
		if i < len(s) && (s[i] == '+' || s[i] == '-') {
			i++
		}
		if digits() == 0 {
			return false
		}
	}
	return i == len(s)
}

// parseOpInt gates an ops int like the reference: grammar first, then range.
func parseOpInt(s string) (int64, error) {
	if !intGrammar(s) {
		return 0, fmt.Errorf("bad int: %s", s)
	}
	n, err := strconv.ParseInt(s, 10, 64)
	if err != nil {
		return 0, fmt.Errorf("bad int: %s", s)
	}
	return n, nil
}

// parseOpFloat gates an ops float; overflow yields +/-Inf like the reference,
// not an error.
func parseOpFloat(s string) (float64, error) {
	if !floatGrammar(s) {
		return 0, fmt.Errorf("bad float: %s", s)
	}
	n, err := strconv.ParseFloat(s, 64)
	if err != nil {
		if ne, ok := err.(*strconv.NumError); !ok || ne.Err != strconv.ErrRange {
			return 0, fmt.Errorf("bad float: %s", s)
		}
	}
	return n, nil
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
	pint := parseOpInt
	pflt := parseOpFloat
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
	wrote := false
	switch f[0] {
	case "int":
		n, err := pint(v)
		if err != nil {
			return err
		}
		wrote = doc.SetInt(path, n)
	case "float":
		n, err := pflt(v)
		if err != nil {
			return err
		}
		wrote = doc.SetFloat(path, n)
	case "bool":
		wrote = doc.SetBool(path, v == "true")
	case "string":
		wrote = doc.SetString(path, unescapeOps(v))
	case "datetime":
		x, err := dt(v)
		if err != nil {
			return err
		}
		wrote = doc.SetDateTime(path, x)
	case "int-default":
		n, err := pint(v)
		if err != nil {
			return err
		}
		wrote = doc.SetIntDefault(path, n)
	case "float-default":
		n, err := pflt(v)
		if err != nil {
			return err
		}
		wrote = doc.SetFloatDefault(path, n)
	case "bool-default":
		wrote = doc.SetBoolDefault(path, v == "true")
	case "string-default":
		wrote = doc.SetStringDefault(path, unescapeOps(v))
	case "datetime-default":
		x, err := dt(v)
		if err != nil {
			return err
		}
		wrote = doc.SetDateTimeDefault(path, x)
	case "int-array":
		xs, err := ints(arr)
		if err != nil {
			return err
		}
		wrote = doc.SetIntArray(path, xs)
	case "float-array":
		xs, err := flts(arr)
		if err != nil {
			return err
		}
		wrote = doc.SetFloatArray(path, xs)
	case "bool-array":
		wrote = doc.SetBoolArray(path, bools(arr))
	case "string-array":
		wrote = doc.SetStringArray(path, strs(arr))
	case "datetime-array":
		xs, err := dts(arr)
		if err != nil {
			return err
		}
		wrote = doc.SetDateTimeArray(path, xs)
	case "int-array-default":
		xs, err := ints(arr)
		if err != nil {
			return err
		}
		wrote = doc.SetIntArrayDefault(path, xs)
	case "float-array-default":
		xs, err := flts(arr)
		if err != nil {
			return err
		}
		wrote = doc.SetFloatArrayDefault(path, xs)
	case "bool-array-default":
		wrote = doc.SetBoolArrayDefault(path, bools(arr))
	case "string-array-default":
		wrote = doc.SetStringArrayDefault(path, strs(arr))
	case "datetime-array-default":
		xs, err := dts(arr)
		if err != nil {
			return err
		}
		wrote = doc.SetDateTimeArrayDefault(path, xs)
	case "raw":
		wrote = doc.SetRaw(path, unescapeOps(get(3)), v)
	case "raw-default":
		wrote = doc.SetRawDefault(path, unescapeOps(get(3)), v)
	case "empty":
		wrote = doc.SetEmpty(path)
	case "comment":
		wrote = doc.SetComment(path, v)
	case "remove":
		doc.Remove(path)
		wrote = true
	default:
		return fmt.Errorf("unknown op: %s", f[0])
	}
	if !wrote {
		return fmt.Errorf("cannot write %s", path)
	}
	return nil
}

func doSet(o *opts) int {
	if len(o.args) != 1 {
		fmt.Fprintln(os.Stderr, "set needs FILE (ops on stdin; see --help)")
		return 1
	}
	file := o.args[0]
	if o.write && file == "-" {
		fmt.Fprintln(os.Stderr, "set --write cannot rewrite stdin; drop --write to print, or pass a FILE")
		return 1
	}
	// Base doc: '-' means an empty base, since stdin carries the ops script.
	// Any --layer files sit under it and --set overrides sit on top, before ops.
	layerTexts := make([]string, 0, len(o.layers)+1)
	for _, lf := range o.layers {
		t, err := readInput(lf)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 1
		}
		layerTexts = append(layerTexts, t)
	}
	base := ""
	if file != "-" {
		t, err := readInput(file)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 1
		}
		base = t
	}
	layerTexts = append(layerTexts, base)
	doc, code := loadDoc(layerTexts[0], o.strictness)
	if doc == nil {
		return code
	}
	for _, t := range layerTexts[1:] {
		over, c := loadDoc(t, o.strictness)
		if over == nil {
			return c
		}
		doc.Merge(over)
	}
	for _, s := range o.sets {
		if !doc.SetString(s[0], s[1]) {
			fmt.Fprintf(os.Stderr, "shcl: cannot write %s (from --set)\n", s[0])
			return 1
		}
	}
	// --set carries the edits, so stdin is left alone: reading it here would
	// block on the console for anyone who passed edits as options.
	var ops []byte
	if len(o.sets) == 0 {
		var err error
		ops, err = io.ReadAll(os.Stdin)
		if err != nil {
			fmt.Fprintf(os.Stderr, "stdin: %s\n", err)
			return 1
		}
		// The reference reads ops via read_to_string; mirror its UTF-8 failure.
		if !utf8.Valid(ops) {
			fmt.Fprintln(os.Stderr, "stdin: invalid UTF-8")
			return 1
		}
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
	canonical := doc.ToCanonical()
	if o.write {
		if werr := writeAtomic(file, canonical); werr != nil {
			fmt.Fprintln(os.Stderr, werr)
			return 1
		}
	} else {
		fmt.Print(canonical)
	}
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
	var diags []shcl.Diagnostic
	strictFailed := false
	if doc, perr := shcl.ParseWith(text, o.strictness); perr != nil {
		diags = perr.(*shcl.LoadError).Diagnostics
		strictFailed = true
	} else {
		diags = doc.Diagnostics()
		// --schema: append validation diagnostics under the same contract. The
		// schema itself always loads at Standard (a program artifact); one that
		// does not load cleanly is a single V099 schema fault.
		if o.schema != "" {
			stext, serr := readInput(o.schema)
			if serr != nil {
				fmt.Fprintln(os.Stderr, serr)
				return 1
			}
			sdoc := shcl.Parse(stext)
			bad := false
			for _, sd := range sdoc.Diagnostics() {
				if sd.Severity == shcl.SeverityError {
					bad = true
				}
			}
			if bad {
				for _, sd := range sdoc.Diagnostics() {
					fmt.Fprintf(os.Stderr, "schema line %d: %s: %s\n", sd.Line, sd.Severity, sd.Message)
				}
				diags = append(diags, shcl.Diagnostic{Line: 0, Severity: shcl.SeverityError, Message: "schema failed to load", Code: "V099"})
			} else {
				diags = append(diags, doc.Validate(sdoc)...)
				diags = shcl.SuppressDeclaredRepeats(sdoc, diags)
			}
		}
	}
	// stdout carries the stable codes - the cross-binding contract. The prose is
	// per-binding voice and goes to stderr (which the differential check drops).
	// A V090-V093 line number is a SCHEMA line (the code table says so); the
	// prose names the file so the two number spaces cannot be confused.
	errors := 0
	for _, d := range diags {
		fmt.Printf("line %d: %s: %s\n", d.Line, d.Severity, d.Code)
		space := "line"
		if strings.HasPrefix(d.Code, "V09") && d.Code != "V099" {
			space = "schema line"
		}
		fmt.Fprintf(os.Stderr, "%s %d: %s: %s\n", space, d.Line, d.Severity, d.Message)
		if d.Severity == shcl.SeverityError {
			errors++
		}
	}
	switch {
	case strictFailed:
		fmt.Printf("strict load failed: %d diagnostic(s)\n", len(diags))
		return 6
	case errors > 0:
		// Loaded, but lines were dropped: nonzero so a CI gate on check catches it.
		fmt.Printf("failed: %d diagnostic(s), %d error(s)\n", len(diags), errors)
		return 6
	default:
		fmt.Printf("ok (%d diagnostic(s))\n", len(diags))
		return 0
	}
}

func doInit(o *opts) int {
	if len(o.args) != 0 {
		fmt.Fprintln(os.Stderr, "init takes no file argument (see --help)")
		return 1
	}
	if o.schema == "" {
		fmt.Fprintln(os.Stderr, "init needs --schema=FILE (see --help)")
		return 1
	}
	stext, err := readInput(o.schema)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	// The schema always loads at Standard - a program artifact, not user data.
	sdoc := shcl.Parse(stext)
	bad := false
	for _, d := range sdoc.Diagnostics() {
		if d.Severity == shcl.SeverityError {
			bad = true
		}
	}
	if bad {
		for _, d := range sdoc.Diagnostics() {
			fmt.Fprintf(os.Stderr, "schema line %d: %s: %s\n", d.Line, d.Severity, d.Message)
		}
		fmt.Fprintln(os.Stderr, "init: schema failed to load")
		// A broken schema is a config-semantics failure, not a usage error:
		// same exit as `check --schema` reporting it.
		return 6
	}
	text, faults := shcl.Generate(sdoc)
	if faults != nil {
		for _, d := range faults {
			fmt.Fprintf(os.Stderr, "schema line %d: %s: %s\n", d.Line, d.Severity, d.Message)
		}
		fmt.Fprintln(os.Stderr, "init: schema has faults")
		return 6
	}
	fmt.Print(text)
	return 0
}

func doEnum(o *opts, wantCount bool) int {
	if len(o.args) != 2 {
		fmt.Fprintln(os.Stderr, "count/instances need FILE and PATH (see --help)")
		return 1
	}
	file, path := o.args[0], o.args[1]
	doc, code := loadLayered(o, file)
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
	asked := askedFor(argv)
	if len(argv) == 0 || asked == "help" || argv[0] == "help" {
		fmt.Print(help)
		if len(argv) == 0 {
			return 1
		}
		return 0
	}
	if asked == "version" || argv[0] == "version" {
		fmt.Printf("shcl %s\n", version)
		return 0
	}
	o, err := parseOpts(argv[1:])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	if code := checkOpts(argv[0], o); code != 0 {
		return code
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
	case "init":
		return doInit(o)
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
