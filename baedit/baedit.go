package main

import (
	"bytes"
	"fmt"
	"io/ioutil"
	"os"
)

var (
	startMarker = []byte(`mark.start=1 `)
	endMarker   = []byte(` mark.end=1`)
)

func main() {
	if len(os.Args) != 3 && len(os.Args) != 2 {
		fmt.Fprintf(os.Stderr, "Usage: baedit zImage ['new kernel command line']\n")
		os.Exit(1)
	}
	zImage := os.Args[1]

	b, err := ioutil.ReadFile(zImage)
	if err != nil {
		panic(err)
	}

	fmt.Printf(">OK: %d bytes read\n", len(b))

	start := bytes.Index(b, startMarker)
	if start == -1 {
		panic("cannot find start marker")
	}
	start += len(startMarker)
	availBytes := bytes.Index(b[start:], endMarker)
	if availBytes == -1 {
		panic("cannot find end marker")
	}

	bootArgs := string(b[start : start+availBytes])
	fmt.Printf("current  bootargs = '%s'\n", bootArgs)

	if len(os.Args) < 3 {
		return
	}

	newBootArgs := os.Args[2]
	l := len(newBootArgs)
	if l > availBytes {
		panic("new command line is too long")
	}

	// pad with spaces
	for i := 0; i < availBytes-l; i++ {
		newBootArgs += " "
	}
	fmt.Printf("replaced bootargs = '%s'\n", newBootArgs)

	for i := 0; i < availBytes; i++ {
		b[start+i] = newBootArgs[i]
	}

	err = ioutil.WriteFile(zImage, b, 0775)
	if err != nil {
		panic(err)
	}
	fmt.Printf(">OK: %d bytes written\n", len(b))
}
