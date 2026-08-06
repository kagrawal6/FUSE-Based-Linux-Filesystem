#!/usr/bin/python3

# write data to numfiles and read it back
# we do small writes to ensure block alignment works

import os
import sys

numfiles = int(sys.argv[1])
filelist = ["file" + str(n + 1) for n in range(numfiles)]
numwrites = int(sys.argv[2])
segment = 100

data = os.urandom(segment * numwrites)

os.chdir("mnt")

# write all the data in segments
fhs = [open(name, "wb") for name in filelist]

for i in range(numwrites):
    for fh in fhs:
        towrite = data[i * segment:(i + 1) * segment]
        fh.write(towrite)

for fh in fhs:
    fh.close()

# read each file back and compare to original data
for name in filelist:
    with open(name, "rb") as file:
        contents = file.read()
        if not data == contents:
            print(f"{name} readback does not match data written")
            exit(1)
            
print("Correct")
exit(0)

