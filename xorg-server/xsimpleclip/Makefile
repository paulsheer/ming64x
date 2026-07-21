CC        = gcc
MINGW_CC  = x86_64-w64-mingw32-gcc
CFLAGS    = -Wall -Wextra -O2 -g
MINGW_INC = -I../hw/xwin/winclipboard
LDFLAGS   = -lxcb -lm

TARGETS   = xsimpleclip x11cliptest win32cliptest.exe dib2png

.PHONY: all generate_images clean

all: $(TARGETS)

xsimpleclip: xsimpleclip.c stb_image.h
	$(CC) $(CFLAGS) -o $@ xsimpleclip.c $(LDFLAGS)

x11cliptest: x11cliptest.c cliptest.h stb_image.h dib.c dib.h
	$(CC) $(CFLAGS) -o $@ x11cliptest.c dib.c -lxcb -lm

dib2png: dib2png.c dib.c dib.h
	$(CC) $(CFLAGS) -I../hw/xwin/winclipboard -o $@ dib2png.c dib.c -lm

win32cliptest.exe: win32cliptest.c cliptest.h stb_image.h dib.c dib.h
	$(MINGW_CC) $(CFLAGS) $(MINGW_INC) -o $@ win32cliptest.c dib.c -lm -lws2_32

# ---------- mask.png generation ----------
mask.png: generate_mask.py
	python3 generate_mask.py

# ---------- test image generation ----------
INPUT_DALMATIAN = input/dalmatian.png

generate_images: mask.png
	@echo "Generating test images..."
	@mkdir -p clip-results
	@for pair in small:200 medium:900 large:3600; do \
		NAME=$${pair%%:*}; W=$${pair##*:}; \
		echo "  size=$$NAME ($${W}x$${W})"; \
		\
		convert $(INPUT_DALMATIAN) -resize $${W}x$${W} \
			\( mask.png -resize $${W}x$${W} \) \
			-compose CopyOpacity -composite \
			input/dalmatian-$${NAME}-circle.png; \
		\
		convert $(INPUT_DALMATIAN) -resize $${W}x$${W} \
			\( mask.png -resize $${W}x$${W} \) \
			-compose CopyOpacity -composite \
			input/dalmatian-$${NAME}-circle.gif; \
		\
		convert $(INPUT_DALMATIAN) -resize $${W}x$${W} -alpha On \
			\( mask.png -resize $${W}x$${W} \) \
			-compose CopyOpacity -composite \
			-background '#808080' -alpha Remove -alpha Off \
			-quality 100 \
			input/dalmatian-$${NAME}-circle.jpg; \
		\
		convert $(INPUT_DALMATIAN) -resize $${W}x$${W} -alpha On \
			\( mask.png -resize $${W}x$${W} \) \
			-compose CopyOpacity -composite \
			-background '#808080' -alpha Remove -alpha Off \
			input/dalmatian-$${NAME}-circle.bmp; \
		\
		convert $(INPUT_DALMATIAN) -resize $${W}x$${W} \
			input/dalmatian-$${NAME}-square.png; \
		\
		convert $(INPUT_DALMATIAN) -resize $${W}x$${W} \
			input/dalmatian-$${NAME}-square.gif; \
		\
		convert $(INPUT_DALMATIAN) -resize $${W}x$${W} \
			-quality 100 \
			input/dalmatian-$${NAME}-square.jpg; \
		\
		convert $(INPUT_DALMATIAN) -resize $${W}x$${W} \
			input/dalmatian-$${NAME}-square.bmp; \
	done
	@echo "Done. Generated $$(ls input/dalmatian-* 2>/dev/null | wc -l) test images."

# ---------- clean ----------
clean:
	rm -f $(TARGETS)
	rm -f results.txt
	rm -rf clip-results/
