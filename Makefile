FILES = pty-shell termdumpimg tmux-undercover windowslike
INSDIR = /usr/local/bin
IMGDIR = /usr/local/share/tmux-undercover

all: pty-shell

pty-shell:
	gcc -o pty-shell pty-shell.c

clean:
	rm -f pty-shell

install: $(FILES)
	mkdir -p $(INSDIR)
	cp $(FILES) $(INSDIR)
	for file in $(FILES); do \
		chmod +rx "$(INSDIR)/$$file"; \
	done
	mkdir -p $(IMGDIR)
	cp bscode.png $(IMGDIR)
	chmod +r "$(IMGDIR)/bscode.png"

uninstall:
	for file in $(FILES); do \
		rm -f "$(INSDIR)/$$file"; \
	done
	rm -rf $(IMGDIR)

.PHONY: all clean install uninstall

