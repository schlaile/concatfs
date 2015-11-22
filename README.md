FUSE: Filesystem in Userspace for easy file concatenation of big files

Files with the string "-concat-" anywhere in the filename are considered 
concatenation description special files.

They contain a file list, which, when mounted as a fuse file system
will turn these files into concatenations of the contents of the
contained files.

e.g.

  file1.avi
  file2.avi
  file3.avi

  bigmovie-concat-file.avi

contents of bigmovie-concat-file.avi:

  file1.avi
  file2.avi
  file3.avi

on seperate lines. Empty lines or lines, which do not resolve to a file where
a stat call succeeds, are ignored.

  gcc -Wall poc_concatfs.c `pkg-config fuse --cflags --libs` -o concatfs

Use with:

  concatfs path-to-source-dir path-to-target-dir [fuse-mount options]

