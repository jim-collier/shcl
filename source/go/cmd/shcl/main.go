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
	"strings"
	"unicode/utf8"

	shcl "github.com/jim-collier/shcl/source/go"
)

// Keep in step with source/rust/Cargo.toml, the canonical version source.
const version = "0.1.0"

const help = `shcl - Simple Hierarchical Config Language (reference CLI)

Usage:
  shcl get [type] [options] FILE PATH    read one value (or array) at a path
  shcl fmt [--write] FILE                print (or rewrite) the canonical form
  shcl check [options] FILE              load and print diagnostics
  shcl count [options] FILE PATH         number of instances at a path
  shcl instances [options] FILE PATH     instance values at a path, one per line

Types (default --string):
  --int --float --bool --datetime --string --raw
  --array                                read the value as an array of the type

Options:
  --default=VALUE                        value to print when the read is not Good
                                         (implies --on-bad=default)
  --on-bad=error|default|flag            error: fail loudly; default: print the
                                         default; flag: print the value anyway and
                                         report via exit code (the default mode)
  --strictness=loose|standard|strict     or 1|2|3 (default standard)

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
	def        string
	onBad      string // error|default|flag
	strictness shcl.Strictness
	write      bool
	args       []string // positional: FILE [PATH]
}

func parseOpts(argv []string) (*opts, error) {
	o := &opts{kind: "string", onBad: "flag", strictness: shcl.Standard}
	for _, a := range argv {
		switch {
		case a == "--int" || a == "--float" || a == "--bool" || a == "--datetime" || a == "--string" || a == "--raw":
			o.kind = a[2:]
		case a == "--array":
			o.array = true
		case a == "--write" || a == "-w":
			o.write = true
		case strings.HasPrefix(a, "--default="):
			o.def = a[len("--default="):]
			o.onBad = "default"
		case strings.HasPrefix(a, "--on-bad="):
			v := a[len("--on-bad="):]
			if v != "error" && v != "default" && v != "flag" {
				return nil, fmt.Errorf("bad --on-bad value: %s", v)
			}
			o.onBad = v
		case strings.HasPrefix(a, "--strictness="):
			v := a[len("--strictness="):]
			s, ok := shcl.StrictnessFromArg(v)
			if !ok {
				return nil, fmt.Errorf("bad --strictness value: %s", v)
			}
			o.strictness = s
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
	if o.array {
		switch o.kind {
		case "int":
			r := doc.ReadIntArray(path)
			for _, v := range r.Value {
				lines = append(lines, fmt.Sprintf("%d", v))
			}
			status = r.Status
		case "float":
			r := doc.ReadFloatArray(path)
			for _, v := range r.Value {
				lines = append(lines, shcl.FormatFloat(v))
			}
			status = r.Status
		case "bool":
			r := doc.ReadBoolArray(path)
			for _, v := range r.Value {
				lines = append(lines, fmt.Sprintf("%t", v))
			}
			status = r.Status
		case "datetime":
			r := doc.ReadDateTimeArray(path)
			for _, v := range r.Value {
				lines = append(lines, v.String())
			}
			status = r.Status
		case "raw":
			fmt.Fprintln(os.Stderr, "--raw has no --array form")
			return 1
		default:
			r := doc.ReadStringArray(path)
			lines = r.Value
			status = r.Status
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
	switch {
	case status == shcl.Good || (status == shcl.Empty && o.onBad == "flag"):
		for _, l := range lines {
			fmt.Println(l)
		}
		return statusCode(status)
	case o.onBad == "default":
		fmt.Println(o.def)
		return 0
	case o.onBad == "error":
		fmt.Fprintf(os.Stderr, "%s: %s\n", path, status)
		return statusCode(status)
	default:
		// flag: print the zero/empty value anyway; the exit code carries the status
		for _, l := range lines {
			fmt.Println(l)
		}
		return statusCode(status)
	}
}

func doFmt(o *opts) int {
	if len(o.args) != 1 {
		fmt.Fprintln(os.Stderr, "fmt needs FILE (see --help)")
		return 1
	}
	file := o.args[0]
	text, err := readInput(file)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	canonical := shcl.Parse(text).ToCanonical()
	if o.write && file != "-" {
		if werr := os.WriteFile(file, []byte(canonical), 0o644); werr != nil {
			fmt.Fprintf(os.Stderr, "%s: %s\n", file, werr)
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
	if len(argv) == 0 || wantsHelp {
		fmt.Print(help)
		if len(argv) == 0 {
			return 1
		}
		return 0
	}
	if wantsVersion {
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
