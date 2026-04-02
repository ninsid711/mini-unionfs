# Makefile — Mini-UnionFS (Team 6)
# Written by: Member 1

CC      = gcc
CFLAGS  = -Wall -Wextra -g $(shell pkg-config --cflags fuse3)
LDFLAGS = $(shell pkg-config --libs fuse3)

TARGET  = mini_unionfs
SRC     = src/mini_unionfs.c src/dir_ops.c src/file_ops.c

# ── Build ──────────────────────────────────────────────────────
$(TARGET): $(SRC) src/mini_unionfs.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

# ── Test ───────────────────────────────────────────────────────
# Runs the automated test suite from the project spec.
# Make sure the filesystem is NOT already mounted before running.
test_run: $(TARGET)
	@chmod +x tests/test_unionfs.sh
	@./tests/test_unionfs.sh

# ── Unmount ───────────────────────────────────────────────────
umount:
	@fusermount3 -u test/mnt 2>/dev/null \
	  || umount test/mnt 2>/dev/null \
	  || echo "Already unmounted or mount not found"

# ── Clean ─────────────────────────────────────────────────────
clean: umount
	rm -f $(TARGET)

.PHONY: test mount umount clean
