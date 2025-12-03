all: spcplay.com

spcplay.com: spcplay.c
	zcc +msx -subtype=msxdos2 --opt-code-speed -o $@ $<

clean:
	$(RM) spcplay.com
