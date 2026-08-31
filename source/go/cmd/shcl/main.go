// SPDX-License-Identifier: MIT
// Copyright © 2026 Jim Collier (CryptogID: ѳ6ᴚ℈𐀘𐇦ɛ𐊁¥Mﾏb϶Δ𐌞)

// shcl CLI - the Go binding's command surface. Flags, output, and exit codes
// mirror the Rust reference exactly; the cicd cross-binding check compares the
// two byte for byte, so any drift here fails the pipeline.
package main

import (
	"errors"
	"fmt"
	"io"
	"os"
	"strconv"
	"strings"
	"unicode/utf8"

	shcl "github.com/jim-collier/shcl/source/go/v2"
)

// Keep in step with source/rust/Cargo.toml, the canonical version source.
const version = "2.0.0"

const help = `shcl - Simple Hierarchical Config Language (reference CLI)

Usage:
  shcl get [type] [options] FILE PATH    read one value (or array) at a path
  shcl set [--write|-w] [options] FILE   apply edits (--set, or ops on stdin);
                                         print canonical (or rewrite FILE in
                                         place with --write)
  shcl fmt [--write|-w] FILE             print the canonical form (or rewrite
                                         FILE in place with --write)
  shcl check [options] FILE              load and print diagnostics
                                         (--schema=SCHEMA also validates FILE
                                         against a schema, itself a .shcl file)
  shcl init [--no-banner] --schema=S     print a commented starter config
                                         from a schema (required fields live,
                                         optional commented, wildcards noted)
  shcl count [options] FILE PATH         number of instances at a path
  shcl instances [options] FILE PATH     instance values at a path, one per line
  shcl children [options] FILE [PATH]    child field names under a path, one per
                                         line (the top level when PATH is left
                                         out)
  shcl paths [options] FILE              every field path in the document, one
                                         per line
  shcl help | version                    this help, or the version (also
                                         -h/--help, -v/-V/--version)
  shcl about | donate                    what shcl is, or how to support it
                                         (also --about, --donate)

set edits FILE, the base document. Values go in as repeatable --set PATH=VALUE
(data) or --set-literal PATH=TEXT (value syntax, so arrays work) options, which
persist with --write; given either, no ops are read from stdin. Raw blocks,
set-only-if-absent and removal go in as a write-ops script on stdin, one op per
line, tab-separated. FILE '-' follows stdin: the document when an option holds
the edits, an empty base when the ops script has stdin instead. With --write,
a FILE that does not exist yet is created. PATH ends at the first '=' outside
quotes and brackets, so a selector may hold one. Ops:
  int|float|bool|string|datetime<TAB>PATH<TAB>VALUE       set a scalar
  <type>-array<TAB>PATH<TAB>V1<TAB>V2...                  set an inline array
  <type>[-array]-default<TAB>...                          set only if absent
  literal[-default]<TAB>PATH<TAB>TEXT                     set from value syntax
  raw<TAB>PATH<TAB>INFO<TAB>CONTENT                       set a raw block
  empty<TAB>PATH   comment<TAB>PATH<TAB>TEXT   remove<TAB>PATH
string/raw values decode \n \t \\; a line starting with # is a script comment.

Types (get only; default --string):
  --int --float --bool --datetime --string --raw --rawinfo
  --array                                read the value as an array of the type
  --rawinfo reads a raw block's info-string (the fence tag), not its content

Options (the subcommands each belongs to are in parentheses):
  --default=VALUE                        (get) value to print when the read is
                                         not Good (implies --on-bad=default; for
                                         arrays, substituted per bad slot)
  --on-bad=error|default|flag            (get) error: fail loudly; default:
                                         print the default; flag: print the
                                         value anyway and report via exit code
                                         (the default)
  --slots                                (get) prefix each line with its slot
                                         status and a tab (per element, or per
                                         wildcard slot)
  --no-banner                            (init) leave out the footer naming the
                                         format and pointing at its spec
  --lossy                                (fmt/set) with --write, rewrite even
                                         when the load dropped lines this write
                                         would delete; without it the write
                                         refuses and nothing is changed
  --strictness=loose|standard|strict     (all but init) or 1|2|3 (default
                                         standard)
  --schema=SCHEMA                        (check/init) validate FILE against a
                                         schema; adds V### diagnostics
  --layer=FILE                           (all but check/init) merge a
                                         lower-priority layer under FILE;
                                         repeatable, earlier = lower priority
  --set=PATH=VALUE                       (all but check/init) override one path
                                         as the top layer, after all files;
                                         repeatable. On 'set' it is an edit to
                                         the document itself, so it persists
                                         with --write. VALUE goes in as data:
                                         its type still follows the text (8 is
                                         an int), but a comma or quote in it is
                                         content, not syntax
  --set-literal=PATH=TEXT                (same subcommands) as --set, except
                                         TEXT goes in as value
                                         syntax the way a file spells it, so
                                         'ports=80, 443' writes a two-element
                                         array. An unquoted # ends the value;
                                         text spanning lines is rejected
  --set-default=PATH=VALUE               (same) as --set, but only when nothing
  --set-literal-default=PATH=TEXT        is at the path yet - the write-out-
                                         defaults half of the writer
  --remove=PATH                          (same) delete what is at the path,
                                         with its subtree. Removing nothing is
                                         not an error
The five above share one ordered list, so two of them touching the same path
resolve in the order given. Raw blocks still go in through the ops script.

Value options accept either spelling: --default=VALUE or --default VALUE. In
the space form the next argument is taken as the value whatever it looks like,
so --default --int reads --int as the default. Use -- to end the options when a
FILE or PATH begins with a dash.
An option a subcommand does not use is a usage error, not ignored. Also
refused: --write with --layer; --write with --set outside 'set'; --lossy
without --write; --layer=- on 'set'; --array with --raw or --rawinfo; '-'
named more than once across FILE, --layer and --schema.
Every subcommand that loads a document prints the load's diagnostics to stderr,
once per run. An in-place write also refuses when the load dropped content the
rewrite would delete (--lossy overrides).
FILE may be '-' for stdin. With --layer, FILE is the highest file layer and
each --layer is merged under it in order; --set applies last. 'fmt' with
layers prints the merged canonical document.

Exit codes: 0 good, 1 usage error, 2 empty, 3 not found, 4 bad type,
5 multiple instances, 6 check failed, strict load failed, or init's schema
has faults, 7 in-place write refused (--lossy overrides), 8 a file or stream
could not be read or written.
`

// About and donate are stdout, so they are byte-for-byte contracts across the
// bindings the same way the help text and the init banner are. The version
// concatenates from the const above so it cannot drift from `shcl version`.
const about = "shcl v" + version + `
Copyright © 2026 Jim Collier (CryptogID: ѳ6ᴚ℈𐀘𐇦ɛ𐊁¥Mﾏb϶Δ𐌞).
Project: https://github.com/jim-collier/shcl
Licensed under the MIT License. Full text at:
  https://spdx.org/licenses/MIT.html
No warranty.

Simple Hierarchical Config Language. Forgiving to write, predictable to read.
Types live in your code, not in the file, so nothing is guessed at parse time.
One broken line is skipped with a note instead of taking down the whole file.
`

const donate = `shcl is free software under the MIT License, and stays that way.

If it saves you time and you want to give something back:
  https://github.com/sponsors/jim-collier

A star on the project, a clear bug report, or a mention to someone who needs it
are worth just as much.
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

// setOpt is one --set/--set-literal override. Both spellings share a list so
// they apply in the order given, which is what decides the winner when two
// target the same path.
// setKind is which spelling produced one edit. They share a single ordered
// list, so two options touching the same path resolve in the order given.
type setKind int

const (
	setData setKind = iota
	setLiteral
	setDataDefault
	setLiteralDefault
	setRemove
)

type setOpt struct {
	path  string
	value string
	kind  setKind
}

func (s setOpt) apply(doc *shcl.Document) bool {
	switch s.kind {
	case setLiteral:
		return doc.SetLiteral(s.path, s.value)
	case setDataDefault:
		return doc.SetStringDefault(s.path, s.value)
	case setLiteralDefault:
		return doc.SetLiteralDefault(s.path, s.value)
	case setRemove:
		// Removing nothing is not a failure, the same as the ops script's
		// `remove`: the point of the option is the path's absence after.
		doc.Remove(s.path)
		return true
	}
	return doc.SetString(s.path, s.value)
}

func (s setOpt) opt() string {
	switch s.kind {
	case setLiteral:
		return "--set-literal"
	case setDataDefault:
		return "--set-default"
	case setLiteralDefault:
		return "--set-literal-default"
	case setRemove:
		return "--remove"
	}
	return "--set"
}

type kind int

const (
	kindInt kind = iota
	kindFloat
	kindBool
	kindDatetime
	kindString
	kindRaw
	kindRawInfo
)

func kindFromOpt(opt string) (kind, bool) {
	switch opt {
	case "--int":
		return kindInt, true
	case "--float":
		return kindFloat, true
	case "--bool":
		return kindBool, true
	case "--datetime":
		return kindDatetime, true
	case "--string":
		return kindString, true
	case "--raw":
		return kindRaw, true
	case "--rawinfo":
		return kindRawInfo, true
	}
	return kindString, false
}

func (k kind) name() string {
	switch k {
	case kindInt:
		return "int"
	case kindFloat:
		return "float"
	case kindBool:
		return "bool"
	case kindDatetime:
		return "datetime"
	case kindRaw:
		return "raw"
	case kindRawInfo:
		return "rawinfo"
	}
	return "string"
}

type onBad int

const (
	onBadError onBad = iota
	onBadDefault
	onBadFlag
)

type opts struct {
	kind       kind
	array      bool
	slots      bool
	def        string
	onBad      onBad
	strictness shcl.Strictness
	write      bool
	lossy      bool
	noBanner   bool
	schema     string
	layers     []string // lower-priority layers, in listed order
	sets       []setOpt // final override layer, in the order given
	args       []string // positional: FILE [PATH]
	seen       []string // canonical names of options given, for per-command validation
}

// askedFor: did the command line ask for one of the informational outputs? Only
// tokens in option position count: the value of a value-taking option and
// anything after `--` are data (a FILE or PATH spelled `-h` needs the `--`
// anyway, since the option parser would refuse it). Scanning values too once
// let a read of a missing path answer with the help text and exit 0.
func askedFor(argv []string) string {
	for i := 0; i < len(argv); i++ {
		a := argv[i]
		switch {
		case a == "-h" || a == "--help":
			return "help"
		case a == "-v" || a == "-V" || a == "--version":
			return "version"
		case a == "--about":
			return "about"
		case a == "--donate":
			return "donate"
		case a == "--":
			return ""
		case a == "--default" || a == "--on-bad" || a == "--strictness" || a == "--schema" ||
			a == "--layer" || a == "--set" || a == "--set-literal" ||
			a == "--set-default" || a == "--set-literal-default" || a == "--remove":
			i++
		}
	}
	return ""
}

// splitSet: PATH=VALUE at the first `=` outside quotes and brackets, so a
// selector holding one (`x[a=b].c=1`) still addresses its instance.
func splitSet(arg string) (string, string, bool) {
	var inQuote byte
	depth := 0
	for i := 0; i < len(arg); i++ {
		b := arg[i]
		if b == '\\' {
			i++
			continue
		}
		switch {
		case inQuote != 0 && b == inQuote:
			inQuote = 0
		case inQuote != 0:
		case b == '"' || b == '\'':
			inQuote = b
		case b == '[':
			depth++
		case b == ']':
			if depth > 0 {
				depth--
			}
		case b == '=' && depth == 0:
			return arg[:i], arg[i+1:], true
		}
	}
	return "", "", false
}

// asciiLower folds A-Z only, mirroring the library helper the strictness option
// already goes through. strings.ToLower folds by Unicode, which is a different
// question from "is this one of three ASCII words".
func asciiLower(s string) string {
	i := 0
	for ; i < len(s); i++ {
		if s[i] >= 'A' && s[i] <= 'Z' {
			break
		}
	}
	if i == len(s) {
		return s
	}
	b := []byte(s)
	for ; i < len(b); i++ {
		if b[i] >= 'A' && b[i] <= 'Z' {
			b[i] += 'a' - 'A'
		}
	}
	return string(b)
}

func setValueOpt(o *opts, name, v string) error {
	switch name {
	case "--default":
		o.def = v
		o.onBad = onBadDefault
		o.seen = append(o.seen, "--default")
	case "--on-bad":
		switch asciiLower(v) {
		case "error":
			o.onBad = onBadError
		case "default":
			o.onBad = onBadDefault
		case "flag":
			o.onBad = onBadFlag
		default:
			return fmt.Errorf("bad --on-bad value: %s", v)
		}
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
	case "--remove":
		if v == "" {
			return fmt.Errorf("bad --remove value (want PATH)")
		}
		o.sets = append(o.sets, setOpt{path: v, kind: setRemove})
		o.seen = append(o.seen, "--remove")
	case "--set", "--set-literal", "--set-default", "--set-literal-default":
		p, val, ok := splitSet(v)
		if !ok || p == "" {
			return fmt.Errorf("bad %s value (want PATH=VALUE, quotes and brackets balanced): %s", name, v)
		}
		k := setData
		switch name {
		case "--set-literal":
			k = setLiteral
		case "--set-default":
			k = setDataDefault
		case "--set-literal-default":
			k = setLiteralDefault
		}
		o.sets = append(o.sets, setOpt{path: p, value: val, kind: k})
		o.seen = append(o.seen, name)
	}
	return nil
}

func parseOpts(argv []string) (*opts, error) {
	o := &opts{kind: kindString, onBad: onBadFlag, strictness: shcl.Standard}
	// Value-taking options accept both --opt=VALUE and the space form --opt VALUE.
	for i := 0; i < len(argv); i++ {
		a := argv[i]
		// Everything after `--` is positional, so a file or path may begin
		// with a dash.
		if a == "--" {
			o.args = append(o.args, argv[i+1:]...)
			return o, nil
		}
		if k, ok := kindFromOpt(a); ok {
			o.kind = k
			o.seen = append(o.seen, "--<type>")
			continue
		}
		switch {
		case a == "--array":
			o.array = true
			o.seen = append(o.seen, "--array")
		case a == "--slots":
			o.slots = true
			o.seen = append(o.seen, "--slots")
		case a == "--write" || a == "-w":
			o.write = true
			o.seen = append(o.seen, "--write")
		case a == "--lossy":
			o.lossy = true
			o.seen = append(o.seen, "--lossy")
		case a == "--no-banner":
			o.noBanner = true
			o.seen = append(o.seen, "--no-banner")
		case a == "--default" || a == "--on-bad" || a == "--strictness" || a == "--schema" ||
			a == "--layer" || a == "--set" || a == "--set-literal" ||
			a == "--set-default" || a == "--set-literal-default" || a == "--remove":
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
		case strings.HasPrefix(a, "--set-literal-default="):
			if err := setValueOpt(o, "--set-literal-default", a[len("--set-literal-default="):]); err != nil {
				return nil, err
			}
		case strings.HasPrefix(a, "--set-literal="):
			if err := setValueOpt(o, "--set-literal", a[len("--set-literal="):]); err != nil {
				return nil, err
			}
		case strings.HasPrefix(a, "--set-default="):
			if err := setValueOpt(o, "--set-default", a[len("--set-default="):]); err != nil {
				return nil, err
			}
		case strings.HasPrefix(a, "--remove="):
			if err := setValueOpt(o, "--remove", a[len("--remove="):]); err != nil {
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
		allowed = []string{"--<type>", "--array", "--slots", "--default", "--on-bad", "--strictness",
			"--layer", "--set", "--set-literal", "--set-default", "--set-literal-default", "--remove"}
	case "set":
		allowed = []string{"--strictness", "--layer", "--set", "--set-literal", "--set-default",
			"--set-literal-default", "--remove", "--write", "--lossy"}
	case "fmt":
		allowed = []string{"--write", "--lossy", "--strictness", "--layer", "--set", "--set-literal",
			"--set-default", "--set-literal-default", "--remove"}
	case "check":
		allowed = []string{"--strictness", "--schema"}
	case "init":
		allowed = []string{"--schema", "--no-banner"}
	case "count", "instances", "children", "paths":
		allowed = []string{"--strictness", "--layer", "--set", "--set-literal", "--set-default",
			"--set-literal-default", "--remove"}
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
			} else if cmd == "init" && s == "--strictness" {
				// Deliberate, not an oversight: the schema is a program artifact,
				// so it always loads at Standard - the same rule `check --schema`
				// follows for the schema half.
				fmt.Fprintln(os.Stderr, "option --strictness not valid for init: a schema always loads at "+
					"standard strictness, being a program artifact rather than user data")
			} else if cmd == "check" && (s == "--layer" || s == "--set" || s == "--set-literal") {
				// The one refusal a user is likely to want anyway: check reports
				// line numbers, and a merged document has no single file to
				// number against. Naming the pipeline turns a dead end into a
				// one-liner.
				fmt.Fprintf(os.Stderr, "option %s not valid for check: diagnostics cite line numbers, "+
					"which a merged document has none of. Pipe instead: "+
					"shcl fmt %s ... FILE | shcl check --schema=SCHEMA -\n", s, s)
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
		fmt.Fprintf(os.Stderr, "--write cannot be combined with %s (see --help)\n", o.sets[0].opt())
		return 1
	}
	// --lossy only overrides the in-place write's refusal, so on its own it says
	// nothing and would read as protection the command never had.
	if o.lossy && !o.write {
		fmt.Fprintln(os.Stderr, "--lossy is only meaningful with --write (see --help)")
		return 1
	}
	// The ops script already has stdin, so a layer cannot read it too.
	if cmd == "set" {
		for _, l := range o.layers {
			if l == "-" {
				fmt.Fprintln(os.Stderr, "--layer=- is not valid for set (stdin carries the ops script or the document)")
				return 1
			}
		}
	}
	// Stdin reads once; a second '-' would silently get an empty document.
	stdinUses := 0
	for _, l := range o.layers {
		if l == "-" {
			stdinUses++
		}
	}
	if o.schema == "-" {
		stdinUses++
	}
	if len(o.args) > 0 && o.args[0] == "-" {
		stdinUses++
	}
	if stdinUses > 1 {
		fmt.Fprintln(os.Stderr, "'-' (stdin) can be named only once across FILE, --layer and --schema")
		return 1
	}
	return 0
}

// describeRefusal is the per-binding wording behind a setter's bare false.
func describeRefusal(doc *shcl.Document, path string) string {
	switch doc.WriteReason(path) {
	case shcl.Writable:
		// The path itself is fine, so the value text must be what failed (a
		// literal that does not parse as one value).
		return "the value text is not one value"
	case shcl.ValueInPath:
		return "a path with a value part cannot be written"
	case shcl.Wildcard:
		return "a wildcard path cannot be written"
	case shcl.NoSuchIndex:
		return "no instance at that index"
	case shcl.TooDeep:
		return "deeper than the nesting cap"
	}
	return "not a usable path" // BadPath
}

// sayDiagnostics prints the load's diagnostics, one line each, in the shape
// every command uses.
func sayDiagnostics(diags []shcl.Diagnostic) {
	for _, d := range diags {
		space := "line"
		if strings.HasPrefix(d.Code, "V09") && d.Code != "V099" {
			space = "schema line"
		}
		fmt.Fprintf(os.Stderr, "%s %d: %s: %s %s\n", space, d.Line, d.Severity, d.Code, d.Message)
	}
}

// exitIO is a file or stream that could not be read or written. Its own code
// since a script's remedy - fix the path, the permissions, the disk - has
// nothing to do with the remedy for a usage error, which keeps 1.
const exitIO = 8

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
		// Checked form: this is the top-level error path, so a future error type
		// here has to report rather than panic.
		if le, ok := err.(*shcl.LoadError); ok {
			sayDiagnostics(le.Diagnostics)
			errorCount := 0
			for _, d := range le.Diagnostics {
				if d.Severity == shcl.SeverityError {
					errorCount++
				}
			}
			fmt.Fprintf(os.Stderr, "strict load failed: %d error diagnostic(s)\n", errorCount)
		} else {
			fmt.Fprintln(os.Stderr, err)
		}
		return nil, 6
	}
	return doc, 0
}

// writeBack is the in-place half of fmt/set. Overwriting the source is the one
// place a recovered load turns destructive, so the diagnostics go out even
// though the command succeeded, and the save runs through the library's own
// gate rather than a second copy of the rule - the CLI and a consumer program
// cannot then disagree about which rewrites are safe.
func writeBack(doc *shcl.Document, file string, o *opts) int {
	var werr error
	if o.lossy {
		werr = doc.SaveFileLossy(file)
	} else {
		werr = doc.SaveFile(file)
	}
	if werr == nil {
		return 0
	}
	// The rule stays in the library; only the wording is the CLI's, because the
	// override a user has here is a flag, not a function.
	var refused *shcl.SaveRefused
	if errors.As(werr, &refused) {
		fmt.Fprintf(os.Stderr, "%s: refusing to rewrite: the load dropped %d line(s)/value(s) "+
			"this write would delete (--lossy overrides)\n", file, refused.Lost)
		return 7
	}
	fmt.Fprintln(os.Stderr, werr)
	return exitIO
}

// loadLayered loads file with o's lower-priority --layer files underneath and
// its --set overrides on top - the layered-load fold. Every layer parses at the
// requested strictness; a strict-load failure on any layer aborts like a
// single-file strict failure (exit 6). Returns (doc, diags, 0) or (nil, nil, code).
//
// The diagnostics come back per layer, lowest first: a merge does not carry
// them over, so reading them off the merged document drops the ones for FILE
// itself, which is the one the caller named.
func loadLayered(o *opts, file string) (*shcl.Document, []shcl.Diagnostic, int) {
	texts := make([]string, 0, len(o.layers)+1)
	for _, lf := range o.layers {
		t, err := readInput(lf)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			return nil, nil, exitIO
		}
		texts = append(texts, t)
	}
	base, err := readInput(file)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return nil, nil, exitIO
	}
	texts = append(texts, base)
	doc, code := loadDoc(texts[0], o.strictness)
	if code != 0 {
		return nil, nil, code
	}
	diags := append([]shcl.Diagnostic(nil), doc.Diagnostics()...)
	for _, t := range texts[1:] {
		over, c := loadDoc(t, o.strictness)
		if c != 0 {
			return nil, nil, c
		}
		diags = append(diags, over.Diagnostics()...)
		doc.Merge(over)
	}
	for _, s := range o.sets {
		if !s.apply(doc) {
			fmt.Fprintf(os.Stderr, "%s: cannot write %s: %s\n", s.opt(), s.path, describeRefusal(doc, s.path))
			return nil, nil, 1
		}
	}
	return doc, diags, 0
}

// doGet: one value read, formatted for the shell: scalars print as one line,
// arrays one element per line.
func doGet(o *opts) int {
	if len(o.args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: shcl get [type] [options] FILE PATH (see --help)")
		return 1
	}
	file, path := o.args[0], o.args[1]
	doc, diags, code := loadLayered(o, file)
	if doc == nil {
		return code
	}
	// A read reports what the load dropped, the same as fmt and set: below
	// strict the value comes back fine and the damage is otherwise silent.
	// One report per invocation, so a read in a loop is one line per call.
	sayDiagnostics(diags)
	var lines []string
	var status shcl.Status
	var slots []shcl.Status
	if o.array {
		switch o.kind {
		case kindInt:
			r := doc.ReadIntArray(path)
			for _, v := range r.Value {
				lines = append(lines, fmt.Sprintf("%d", v))
			}
			status = r.Status
			slots = r.Slots
		case kindFloat:
			r := doc.ReadFloatArray(path)
			for _, v := range r.Value {
				lines = append(lines, shcl.FormatFloat(v))
			}
			status = r.Status
			slots = r.Slots
		case kindBool:
			r := doc.ReadBoolArray(path)
			for _, v := range r.Value {
				lines = append(lines, fmt.Sprintf("%t", v))
			}
			status = r.Status
			slots = r.Slots
		case kindDatetime:
			r := doc.ReadDateTimeArray(path)
			for _, v := range r.Value {
				lines = append(lines, v.String())
			}
			status = r.Status
			slots = r.Slots
		case kindRaw, kindRawInfo:
			fmt.Fprintf(os.Stderr, "--%s has no --array form\n", o.kind.name())
			return 1
		default:
			r := doc.ReadStringArray(path)
			lines = r.Value
			status = r.Status
			slots = r.Slots
		}
	} else {
		switch o.kind {
		case kindInt:
			r := doc.ReadInt(path)
			lines = []string{fmt.Sprintf("%d", r.Value)}
			status = r.Status
		case kindFloat:
			r := doc.ReadFloat(path)
			lines = []string{shcl.FormatFloat(r.Value)}
			status = r.Status
		case kindBool:
			r := doc.ReadBool(path)
			lines = []string{fmt.Sprintf("%t", r.Value)}
			status = r.Status
		case kindDatetime:
			r := doc.ReadDateTime(path)
			lines = []string{r.Value.String()}
			status = r.Status
		case kindRaw:
			r := doc.ReadRaw(path)
			lines = []string{r.Value}
			status = r.Status
		case kindRawInfo:
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
	// Why the read failed is worth saying even when the exit code already
	// carries it: at the default mode the user otherwise gets an empty line, a
	// nonzero code, and nothing to go on. Stdout is untouched - this only ever
	// goes to stderr. Two silences are deliberate: default mode, because a
	// caller who supplied a fallback has already said the miss is expected, and
	// Empty outside error mode, because an empty value is a legitimate answer
	// here rather than a failure - the same reason Ok counts it as fine.
	if status != shcl.Good && o.onBad != onBadDefault && (status != shcl.Empty || o.onBad == onBadError) {
		typeName := o.kind.name()
		if o.array {
			typeName = o.kind.name() + " array"
		}
		var reason string
		switch status {
		case shcl.BadType:
			if raw := doc.ReadString(path).Raw; raw != nil {
				reason = fmt.Sprintf("value %s is not a valid %s", quoted(*raw), typeName)
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
		fmt.Fprintf(os.Stderr, "cannot read %s as %s: %s (in %s)\n", path, typeName, reason, file)
	}
	switch {
	case status == shcl.Good || (status == shcl.Empty && o.onBad == onBadFlag):
		emit(lines)
		return statusCode(status)
	case o.onBad == onBadDefault:
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
	case o.onBad == onBadError:
		// The message already went to stderr above; error mode differs only in
		// printing nothing on stdout.
		return statusCode(status)
	default:
		// print the zero/empty value anyway; the exit code carries the status
		emit(lines)
		return statusCode(status)
	}
}

// quoted is the source text, quoted for a message: one line whatever it holds,
// with the same escapes in every binding.
func quoted(s string) string {
	var b strings.Builder
	b.Grow(len(s) + 2)
	b.WriteByte('"')
	for _, c := range s {
		switch {
		case c == '"':
			b.WriteString("\\\"")
		case c == '\\':
			b.WriteString("\\\\")
		case c == '\n':
			b.WriteString("\\n")
		case c == '\r':
			b.WriteString("\\r")
		case c == '\t':
			b.WriteString("\\t")
		case c < 0x20 || c == 0x7f:
			fmt.Fprintf(&b, "\\u{%x}", c)
		default:
			b.WriteRune(c)
		}
	}
	b.WriteByte('"')
	return b.String()
}

func doFmt(o *opts) int {
	if len(o.args) != 1 {
		fmt.Fprintln(os.Stderr, "usage: shcl fmt [--write|-w] [options] FILE (see --help)")
		return 1
	}
	file := o.args[0]
	if o.write && file == "-" {
		fmt.Fprintln(os.Stderr, "fmt --write cannot rewrite stdin; drop --write to print, or pass a FILE")
		return 1
	}
	doc, diags, code := loadLayered(o, file)
	if doc == nil {
		return code
	}
	// Printing the canonical form drops what the load dropped, the same as a
	// rewrite does, so the diagnostics go out either way.
	sayDiagnostics(diags)
	if o.write {
		return writeBack(doc, file, o)
	}
	fmt.Print(doc.ToCanonical())
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
	low := asciiLower(s)
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
	pbool := func(s string) (bool, error) {
		switch s {
		case "true":
			return true, nil
		case "false":
			return false, nil
		}
		return false, fmt.Errorf("bad bool: %s", s)
	}
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
	bools := func(xs []string) ([]bool, error) {
		out := make([]bool, len(xs))
		for i, s := range xs {
			b, err := pbool(s)
			if err != nil {
				return nil, err
			}
			out[i] = b
		}
		return out, nil
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
		b, err := pbool(v)
		if err != nil {
			return err
		}
		wrote = doc.SetBool(path, b)
	case "string":
		wrote = doc.SetString(path, unescapeOps(v))
	case "datetime":
		x, err := dt(v)
		if err != nil {
			return err
		}
		wrote = doc.SetDateTime(path, x)
	case "literal":
		wrote = doc.SetLiteral(path, v)
	case "literal-default":
		wrote = doc.SetLiteralDefault(path, v)
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
		b, err := pbool(v)
		if err != nil {
			return err
		}
		wrote = doc.SetBoolDefault(path, b)
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
		xs, err := bools(arr)
		if err != nil {
			return err
		}
		wrote = doc.SetBoolArray(path, xs)
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
		xs, err := bools(arr)
		if err != nil {
			return err
		}
		wrote = doc.SetBoolArrayDefault(path, xs)
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
		return fmt.Errorf("cannot write %s: %s", path, describeRefusal(doc, path))
	}
	return nil
}

func doSet(o *opts) int {
	if len(o.args) != 1 {
		fmt.Fprintln(os.Stderr, "usage: shcl set [--write|-w] [options] FILE (see --help)")
		return 1
	}
	file := o.args[0]
	if o.write && file == "-" {
		fmt.Fprintln(os.Stderr, "set --write cannot rewrite stdin; drop --write to print, or pass a FILE")
		return 1
	}
	// Base doc: with the edits given as options no ops script is read, so a '-'
	// file is the document on stdin the way it is everywhere else; only when
	// stdin is the ops script does '-' mean an empty base. Reading neither threw
	// a piped document away at exit 0.
	// Any --layer files sit under it and --set overrides sit on top, before ops.
	layerTexts := make([]string, 0, len(o.layers)+1)
	for _, lf := range o.layers {
		t, err := readInput(lf)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			return exitIO
		}
		layerTexts = append(layerTexts, t)
	}
	// --write names the file this command produces, so a FILE that is not there
	// yet is a create and the edits land in a new document. Only under --write,
	// and only when nothing is at the path at all: without --write there is
	// nothing to create, and a file that exists but cannot be read is still an
	// error rather than something to quietly write over.
	creating := false
	if o.write && file != "-" {
		if _, serr := os.Stat(file); serr != nil {
			creating = true
		}
	}
	base := ""
	if !creating && (file != "-" || len(o.sets) > 0) {
		t, err := readInput(file)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			return exitIO
		}
		base = t
	}
	layerTexts = append(layerTexts, base)
	doc, code := loadDoc(layerTexts[0], o.strictness)
	if doc == nil {
		return code
	}
	diags := append([]shcl.Diagnostic(nil), doc.Diagnostics()...)
	for _, t := range layerTexts[1:] {
		over, c := loadDoc(t, o.strictness)
		if over == nil {
			return c
		}
		diags = append(diags, over.Diagnostics()...)
		doc.Merge(over)
	}
	for _, s := range o.sets {
		if !s.apply(doc) {
			fmt.Fprintf(os.Stderr, "%s: cannot write %s: %s\n", s.opt(), s.path, describeRefusal(doc, s.path))
			return 1
		}
	}
	// --set carries the edits, so stdin is left alone: reading it here would
	// block on the console for anyone who passed edits as options.
	var ops []byte
	if len(o.sets) == 0 {
		var err error
		// Say so before blocking. With nothing on stdin this used to sit there
		// silently, which reads as a hang rather than as a prompt; the note is
		// unconditional so a pipeline and a terminal behave identically. The
		// program-name prefix marks it as a notice; errors carry none.
		fmt.Fprintln(os.Stderr, "shcl: reading write-ops from stdin (one op per line, tab-separated; end with EOF)")
		ops, err = io.ReadAll(os.Stdin)
		if err != nil {
			fmt.Fprintf(os.Stderr, "stdin: %s\n", err)
			return exitIO
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
	sayDiagnostics(diags)
	if o.write {
		return writeBack(doc, file, o)
	}
	fmt.Print(doc.ToCanonical())
	return 0
}

func doCheck(o *opts) int {
	if len(o.args) != 1 {
		fmt.Fprintln(os.Stderr, "usage: shcl check [options] FILE (see --help)")
		return 1
	}
	text, err := readInput(o.args[0])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return exitIO
	}
	var diags []shcl.Diagnostic
	strictFailed := false
	if doc, perr := shcl.ParseWith(text, o.strictness); perr != nil {
		if le, ok := perr.(*shcl.LoadError); ok {
			diags = le.Diagnostics
		}
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
				return exitIO
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
					fmt.Fprintf(os.Stderr, "schema line %d: %s: %s %s\n", sd.Line, sd.Severity, sd.Code, sd.Message)
				}
				diags = append(diags, shcl.Diagnostic{
					Line: 0, Severity: shcl.SeverityError, Message: "schema failed to load", Code: "V099",
				})
			} else {
				diags = append(diags, doc.Validate(sdoc)...)
				diags = shcl.SuppressDeclaredRepeats(sdoc, diags)
				diags = shcl.SuppressDeclaredReopens(sdoc, diags)
			}
		}
	}
	// stdout carries the stable codes - the cross-binding contract. The prose is
	// per-binding voice and goes to stderr (which the differential check drops).
	// A V090-V093 line number is a SCHEMA line (the code table says so); the
	// prose names the file so the two number spaces cannot be confused.
	errorCount := 0
	for _, d := range diags {
		fmt.Printf("line %d: %s: %s\n", d.Line, d.Severity, d.Code)
		if d.Severity == shcl.SeverityError {
			errorCount++
		}
	}
	sayDiagnostics(diags)
	switch {
	case strictFailed:
		fmt.Printf("strict load failed: %d diagnostic(s)\n", len(diags))
		return 6
	case errorCount > 0:
		// Loaded, but lines were dropped: nonzero so a CI gate on check catches it.
		fmt.Printf("failed: %d diagnostic(s), %d error(s)\n", len(diags), errorCount)
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
		return exitIO
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
			fmt.Fprintf(os.Stderr, "schema line %d: %s: %s %s\n", d.Line, d.Severity, d.Code, d.Message)
		}
		fmt.Fprintln(os.Stderr, "init: schema failed to load")
		// A broken schema is a config-semantics failure, not a usage error:
		// same exit as `check --schema` reporting it.
		return 6
	}
	text, faults := shcl.Generate(sdoc, o.noBanner)
	if faults != nil {
		for _, d := range faults {
			fmt.Fprintf(os.Stderr, "schema line %d: %s: %s %s\n", d.Line, d.Severity, d.Code, d.Message)
		}
		fmt.Fprintln(os.Stderr, "init: schema has faults")
		return 6
	}
	fmt.Print(text)
	return 0
}

func doEnum(o *opts, wantCount bool) int {
	if len(o.args) != 2 {
		name := "instances"
		if wantCount {
			name = "count"
		}
		fmt.Fprintf(os.Stderr, "usage: shcl %s [options] FILE PATH (see --help)\n", name)
		return 1
	}
	file, path := o.args[0], o.args[1]
	doc, diags, code := loadLayered(o, file)
	if doc == nil {
		return code
	}
	// A read reports what the load dropped, the same as fmt and set: below
	// strict the value comes back fine and the damage is otherwise silent.
	// One report per invocation, so a read in a loop is one line per call.
	sayDiagnostics(diags)
	if wantCount {
		fmt.Println(doc.Count(path))
	} else {
		for _, v := range doc.Instances(path) {
			fmt.Println(v)
		}
	}
	return 0
}

// doChildren: child field names under a path, one per line, in file order and
// with duplicates kept. PATH may be left out to enumerate the top level. Each
// name comes out in the form a path accepts, so one holding a dot or a quote
// splices back into a path with no further work.
func doChildren(o *opts) int {
	var file, path string
	switch len(o.args) {
	case 1:
		file = o.args[0]
	case 2:
		file, path = o.args[0], o.args[1]
	default:
		fmt.Fprintln(os.Stderr, "usage: shcl children [options] FILE [PATH] (see --help)")
		return 1
	}
	doc, diags, code := loadLayered(o, file)
	if doc == nil {
		return code
	}
	sayDiagnostics(diags)
	for _, name := range doc.Children(path) {
		fmt.Println(shcl.QuoteSegment(name))
	}
	return 0
}

// doPaths: every field path in the document, one per line, in file order and
// deduplicated - the whole-document counterpart of doChildren.
func doPaths(o *opts) int {
	if len(o.args) != 1 {
		fmt.Fprintln(os.Stderr, "usage: shcl paths [options] FILE (see --help)")
		return 1
	}
	doc, diags, code := loadLayered(o, o.args[0])
	if doc == nil {
		return code
	}
	sayDiagnostics(diags)
	for _, p := range doc.Paths() {
		fmt.Println(p)
	}
	return 0
}

var commands = [...]string{"get", "set", "fmt", "check", "init", "count", "instances", "children", "paths"}

func run() int {
	argv := os.Args[1:]
	for _, a := range argv {
		if !utf8.ValidString(a) {
			fmt.Fprintln(os.Stderr, "invalid argument encoding (expected UTF-8)")
			return 1
		}
	}
	asked := askedFor(argv)
	// One convention: asking for the help - by name, by flag, or by asking for
	// nothing at all - prints it and succeeds. The blank lines separate the
	// block from the surrounding prompts. A bare run used to print the same
	// text unpadded and exit 1, which read as neither a help nor an error.
	if len(argv) == 0 {
		fmt.Printf("\n%s\n", help)
		return 0
	}
	if asked == "help" || argv[0] == "help" {
		fmt.Printf("\n%s\n", help)
		return 0
	}
	if asked == "version" || argv[0] == "version" {
		fmt.Printf("shcl %s\n", version)
		return 0
	}
	if asked == "about" || argv[0] == "about" {
		fmt.Printf("\n%s\n", about)
		return 0
	}
	if asked == "donate" || argv[0] == "donate" {
		fmt.Printf("\n%s\n", donate)
		return 0
	}
	cmd := argv[0]
	known := false
	for _, c := range commands {
		if c == cmd {
			known = true
			break
		}
	}
	if !known {
		// Before the options are judged, so a typo in the command is reported
		// as that and not as an option the wrong command cannot take.
		if strings.HasPrefix(cmd, "-") && cmd != "--" {
			fmt.Fprintf(os.Stderr, "unknown option: %s (see --help)\n", cmd)
		} else {
			fmt.Fprintf(os.Stderr, "unknown command: %s (see --help)\n", cmd)
		}
		return 1
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
	case "children":
		return doChildren(o)
	case "paths":
		return doPaths(o)
	default:
		return doEnum(o, false)
	}
}

func main() {
	os.Exit(run())
}
