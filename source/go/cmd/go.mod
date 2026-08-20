// Its own module on purpose, so the CLI does NOT ship inside the library
// module: the go tool leaves any subdirectory holding a go.mod out of the
// parent's zip. The Rust binary is the only CLI this project distributes; the
// other three exist to be driven by the cross-binding differential check.
//
// The replace is what makes it unpublishable as well as unshipped - `go install
// <path>@<version>` refuses a module that needs a replace directive - and it is
// how a local build resolves the library next door rather than the proxy.

module github.com/jim-collier/shcl/source/go/cmd

go 1.20

require github.com/jim-collier/shcl/source/go v0.0.0

replace github.com/jim-collier/shcl/source/go => ../
