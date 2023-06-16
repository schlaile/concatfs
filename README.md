FUSE: Filesystem in Userspace for easy file concatenation of big files

Files with the string "-concat-" anywhere in the filename are considered 
concatenation description special files.

They contain a file list, which, when mounted as a fuse file system
will turn these files into concatenations of the contents of the
contained files.

e.g.

```
  file1.MTS
  file2.MTS
  file3.MTS

  bigmovie-concat-file.MTS
```

contents of bigmovie-concat-file.MTS:

```
file1.MTS
file2.MTS
file3.MTS
```

on seperate lines. Empty lines or lines, which do not resolve to a file where
a stat call succeeds, are ignored.

Simple "range" expressions are supported in -concat- files. This way, you can slice an input file (and even reorder it!). \
The line format is `path:offset:length`, with both `offset` and `length` being optional. \
Do remember that the same file can be specified multiple times, and it will get concatenated multiple times, as expected. \
The order in which the new file is created is the same as in the -concat- file.

Some examples:
```
file1.bin           // use the entire file
file1.bin:10        // use file1.bin again, but starting at offset 10
file2.bin:50:100    // use file2.bin, but only 100 bytes starting at offset 50 (inclusive).
file3.bin::1        // use file3.bin, but only the very first byte
file1.bin           // use file1.bin in its entirety again
```

You will need to install libfuse-dev to compile:

```
sudo apt-get install libfuse-dev
```

Compile with

```
  gcc -Wall concatfs.c `pkg-config fuse --cflags --libs` -o concatfs
```

Use with:

```
  concatfs path-to-source-dir path-to-target-dir [fuse-mount options]
```

