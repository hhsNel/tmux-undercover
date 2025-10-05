FILES = pty-shell termdumpimg tmux-undercover tmux-undercover-stop windowslike
INSDIR = /usr/local/bin

all: pty-shell

pty-shell:
	gcc -o pty-shell pty-shell.c

clean:
	rm -f pty-shell

install: $(FILES)
	mkdir -p $(INSDIR)
	cp $(FILES) $(INSDIR)

uninstall:
	for file in $(FILES); do \
		rm -f "$(INSDIR)/$$file"; \
	done

.PHONY: all clean install uninstall

