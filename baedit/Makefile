all: baedit baedit-ppc

baedit: baedit.go
	CGO_ENABLED=0 GOOS=linux go build -a -ldflags '-s -w -extldflags "-static"' .

baedit-ppc: baedit.go
	powerpc-linux-gnu-gccgo -obaedit-ppc -static baedit.go

clean:
	rm -f baedit baedit-ppc

.PHONY: all clear
