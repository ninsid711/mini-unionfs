# Mini-UnionFS — Team 6

A simplified Union Filesystem in userspace using FUSE (libfuse3), 
written in C as part of the Cloud Computing Mini Project.

## Building

```bash
sudo apt install build-essential libfuse3-dev fuse3 pkg-config
make
```

## Running
Before running, create three directories :
```bash
cd ~/mini-unionfs
mkdir -p test/lower test/upper test/mnt
```
And add some basic text files :
```bash
echo "Hello from lower" > test/lower/base.txt
echo "delete me" > test/lower/delete_me.txt
```
Then run:
```bash
./mini_unionfs test/lower test/upper test/mnt [-f]
# -f runs in foreground (recommended while developing)
```

## Testing

```bash
make test
```

## Unmounting

```bash
make umount
# or manually: fusermount3 -u <mount_point>
```

## Work Distribution

| Member | Area |
|--------|------|
| 1 | Init, path resolution, getattr, Makefile |
| 2 | open, read, write (Copy-on-Write) |
| 3 | readdir (merged directory listing) |
| 4 | create, unlink (whiteout), mkdir, rmdir |