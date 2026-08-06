#!/usr/bin/python3

import sys
import os

numfiles = int(sys.argv[1])
filelist = ["file" + str(n + 1) for n in range(numfiles)]
foundfiles = os.listdir("mnt")
if sorted(filelist) == sorted(foundfiles):
    print("Correct")
    exit(0)
else:
    print("readdir files don't match expectation")
    exit(1)
    
