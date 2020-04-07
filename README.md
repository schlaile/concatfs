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

