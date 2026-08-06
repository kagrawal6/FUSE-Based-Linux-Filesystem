(defun write-test-files (test testnum)
  "Write all the files for a test."
  (let ((testdir "tests"))
    (make-directory testdir :parents)
    (dolist (pair test)
      (with-temp-file (format "%s/%d.%s" testdir testnum (car pair))
	(insert (format "%s" (cdr pair)))))))
  
(defun generate-tests (tests)
  "Accept a list of all project tests, alists and write test files."
  (let ((counter 1))
    (dolist (test tests)
      (let ((testgen (alist-get 'testcase test))
            (configs (alist-get 'configs test)))
	(dolist (config configs)
	  (write-test-files (apply testgen config) counter)
	  (setq counter (1+ counter)))))))

(defun disk-path (img)
  "Generate path for disk image."
  (format "/tmp/$(whoami)/%s" img))

(defun gen-disks (numdisks)
  "Generate string with list of disks for testing."
  (mapcar (lambda (n)
	    (disk-path (format "test-disk%d" n)))
	  (number-sequence 1 numdisks)))

(defun create-disk-cmd (numdisks size)
  "Return command to create disks."
  (mapconcat (lambda (disk)
	       (format "truncate -s %s %s" size disk))
	     (gen-disks numdisks)
	     "; "))

(defun make-mkfs-args (raid numdisks inodes blocks)
  "Generate mkfs args string."
  (let* ((disk-params
	  (mapconcat (lambda (disk) (format "-d %s" disk))
		     (gen-disks numdisks) " "))
	 (base-cmd
	  (format "%s -i %d -b %d" disk-params inodes blocks)))
    (if (< numdisks 2) ; skip raid if one disk
	base-cmd
      (format "-r %s %s" raid base-cmd))))

(defun default-fs-mkfs-args (raid numdisks)
  "Return mkfs args for default fs image."
  (make-mkfs-args raid numdisks 32 200))

(defun small-fs-mkfs-args (raid numdisks)
  "Return mkfs args for small fs image."
  (make-mkfs-args raid numdisks 32 32))

(defun large-fs-mkfs-args (raid numdisks)
  "Return mkfs args for large fs image."
  (make-mkfs-args raid numdisks 128 1024))

(defun py-open-and-write-file (name numbytes)
  "Python to open a file for writing and write numbytes to it."
;  (concat
   (format
    "with open(\"%s\", \"wb\") as f:\n    f.write(b'\\''a'\\'' * %d)\n"
    name
    numbytes))
;   (format "os.stat(\"%s\").st_size\n" name)))

(defun py-try-except (expr)
  "Wrap a python expr in a try.. except block."
  (format
   "
try:
    %s
except Exception as e:
    print(e)
    exit(1)
"
   expr))

(defun py-mknod (name)
  "Python mknod name wrapped in try.. except."
  (py-try-except (format "os.mknod(\"%s\")" name)))

(defun py-mkdir (name)
  "Python mkdir name wrapped in try.. except."
  (py-try-except (format "os.mkdir(\"%s\")" name)))

(defun py-chdir (path)
  "Python chdir name wrapped in try.. except."
  (py-try-except (format "os.chdir(\"%s\")" path)))

(defun py-stat (path isdir)
  "Python stat. We just use to check existence and filetype. Wrapped in try.. except."
  (py-try-except
   (format
    "%s(os.stat(\"%s\").st_mode)"
    (if isdir "S_ISDIR" "S_ISREG") path)))

(defun create-fs-state (mountpoint fs-state prefix)
  "Generate python commands to create filesystem state.

Filesystem state is expanded from FS-STATE.

For example (proper escapes and quotations omitted):
'(())             os.chdir('mnt') os.mkdir('d1')
'((())            os.chdir('mnt') os.mkdir('d1') os.mkdir('d1/d2')
'((file1 . 1024)) with open('file1', wb) as f: f.write(a\0 * 1024)

We also wrap them in try.. except to catch common errors.

MOUNTPOINT the directory under which the filesystem is mounted.
FS-STATE a list describing the state of the filesystem.
PREFIX directory names are always PREFIX and a number."
  (let ((commands '())
        (counter 1))
    (push (format "python3 -c '") commands)
    (push (format "import os\n") commands)
    (push (format "from stat import *\n") commands)
    (push (py-chdir mountpoint) commands)
    (cl-labels ((process-list (lst path)
                 (dolist (item lst)
		   (cond
		    ((not (listp (cdr item)))
		     (let ((filename (if (string= path "")
					 (car item)
				       (format "%s/%s" path (car item)))))
		       (if (not (zerop (cdr item)))
			   (push (py-open-and-write-file
				  filename
				  (cdr item))
				 commands)
			 (push (py-mknod filename) commands))
		       (push (py-stat filename nil) commands)))
		    ((listp item)
                     (let* ((dir (format "%s%d" prefix counter))
			    (fullpath
			     (if (string= path "")
				 dir
			       (format "%s/%s" path dir))))
                       (push (py-mkdir fullpath) commands)
		       (push (py-stat fullpath t) commands)
                       (setq counter (1+ counter))
                       (process-list item fullpath)))))))
      (process-list fs-state ""))
    (push (format "\nprint(\"Correct\")' \\\n") commands)
    (nreverse commands)))

(defun requires-indirect (size)
  (> size 3584))

(defun count-metadata (fs-state numdisks)
  "Generates an alist of expected metadata from a list denoting fs state.

Assumes blocksize is 512 bytes, and directories may hold 16 entries per block.
Returns the expected number of data blocks, directory inodes, and file inodes.

FS-STATE the state of the filesystem."
  (let ((directories 1)
	(files 0)
	(blocks 0)
	(indirect-adjust 0))
    (cl-labels
	((process-list (lst acc)
	   (dolist (item lst)
	     (if (or (= acc 0) (= (mod acc 16) 0)) ;dirents
		 (setq blocks (1+ blocks)))
	     (setq acc (1+ acc))
	     (cond
	      ((not (listp (cdr item))) ;; file
	       (setq files (1+ files))
	       (if (not (zerop (cdr item))) ;; non-empty file
		   (let ((fileblocks
			  (+ (/ (+ (roundup (cdr item) 512)) 512)
			     (if (requires-indirect (cdr item))
				 1
			       0)))) ;ind
		     (setq blocks (+ blocks fileblocks))
		     (setq indirect-adjust
			   (if (requires-indirect (cdr item))
			       (+ indirect-adjust (- numdisks 1))
			     indirect-adjust)))))
	      ((listp item) ;; directory
	       (setq directories (1+ directories))
	       (process-list item 0))))))
      (process-list fs-state 0))
    `((dir-inodes . ,directories)
      (file-inodes . ,files)
      (blocks . ,blocks)
      (indirect-adjust . ,indirect-adjust))))

(defun fs-state-cmds (fs-state prefix)
  "Format fs structure commands into a single string for execution."
  (string-join
   (create-fs-state "mnt" fs-state prefix)))

(defmacro define-test (desc pre post run out pre-rc run-rc err)
  "Generate an alist containing all required test files."
  `(list (cons 'desc (format "%s\n" desc))
	 (cons 'pre (format "%s\n" ,pre))
	 (cons 'post (format "%s\n" ,post))
	 (cons 'run (format "%s\n" ,run))
	 (cons 'out (format "%s\n" ,out))
	 (cons 'pre-rc (format "%s\n" ,pre-rc))
	 (cons 'run-rc (format "%s\n" ,run-rc))
	 (cons 'err ,err)))

(defun roundup (num k)
  "Round num up to the nearest k."
  (let ((remain (mod num k)))
    (if (= remain 0)
	num
      (+ num (- k remain)))))

(defun setup-cmd (numdisks raid)
  "This is always the pre command for filesystem tests.

It creates disks, runs mkfs on them, and mounts with FUSE."
  (string-join
   (list
    "mkdir -p mnt; mkdir -p /tmp/$(whoami)"
    (create-disk-cmd numdisks "1M")
    (concat "../solution/mkfs " (default-fs-mkfs-args raid numdisks))
    (mount-cmd numdisks "mnt"))
   " && ")) ; will stop and return pre-rc if anything goes wrong

(defun teardown-cmd ()
  "This is always the post command for filesystem tests.

It removes the test disks."
  (format "fusermount -uq mnt; rm -f %s"
	  (disk-path "test-disk*")))

(defun mount-cmd (numdisks dir)
  "Mount wfs using NUMDISKS disks in single-threaded mode on DIR.

NUMDISKS the number of disks used for testing
DIR the mount directory"
  (make-directory dir :parents)
  (format
   "../solution/wfs %s -s %s"
   (string-join (gen-disks numdisks) " ")
   dir))

(defun umount-cmd (dir)
  "Un-mount DIR with fusermount.

DIR a directory mounted with FUSE."
  (format "fusermount -u %s" dir))

(defun mkfs-test (desc raid numdisks inodes blocks output pre-rc run-rc)
  "Test template for mfks.

DESC description of the test
RAID raid mode as string (0, 1, or 1v)
NUMDISKS number of disks in the filesystem
INODES number of inodes passed to mkfs
BLOCKS number of blocks passed to mkfs
OUTPUT expected output (usually \"Correct\"
PRE-RC return code of pre command (truncate disks and mkfs)
RUN-RC return code of run command (metadata verifier)"
  (define-test
   (concat "mkfs: " desc)
   (string-join
    (list
     "mkdir -p /tmp/$(whoami)"
     (create-disk-cmd numdisks "1M")
     (concat "../solution/mkfs " (make-mkfs-args raid numdisks inodes blocks)))
    "; ")
   (format "rm -f %s" (disk-path "test-disk*"))
   (format "./wfs-check-metadata.py --mode mkfs --inodes %d --blocks %d --disks %s"
	   (roundup inodes 32)
	   (roundup blocks 32)
	   (string-join (gen-disks numdisks) " "))
   output pre-rc run-rc ""))

(defun verify-metadata-cmd (fs-state extra-blocks numdisks)
  (let ((metadata (count-metadata fs-state numdisks)))
      (format
       "./wfs-check-metadata.py --mode raid%s --blocks %d --altblocks %d --dirs %d --files %d --disks %s"
       raid
       (+ (alist-get 'blocks metadata) extra-blocks)
       (+ (alist-get 'blocks metadata) (alist-get 'indirect-adjust metadata))
       (alist-get 'dir-inodes metadata)
       (alist-get 'file-inodes metadata)
       (string-join (gen-disks numdisks) " "))))

(defun filesystem-init (desc fs-state raid numdisks output rc)
  "Test template for filesystem initialization.

We initialize the filesystem according to FS-STATE, which is compiled
into a python script. This function returns a alist with elements for
each component of a test -- pre, post, run, out, etc. These tests
verify the metadata of each disk image after initializing the filesystem
with FS-STATE.

DESC test description.
NUMDISKS the number of disks to create, at least two.
RAID raid mode as string (0, 1, or 1v)
FS-STATE a list describing the filesystem state
OUTPUT the expected output. Generally \"Correct\" or an error."
  (define-test
   desc
   (setup-cmd numdisks raid)
   (teardown-cmd)
   (concat
    (fs-state-cmds fs-state "d")
    " && "
    (umount-cmd "mnt")
    " && "
    (verify-metadata-cmd fs-state 0 numdisks))
   output
   "0" rc "")) ; pre-rc should always be 0

(defun filesystem-init-and-workload
    (desc fs-state op post-state post-extra-blocks raid numdisks output rc)
    "Test template for filesystem initialization and a workload.

We initialize the filesystem according to FS-STATE, which is compiled
into a python script. Then, we run a workload and verify the filesystem
matches some POST-STATE. This function returns a alist with elements for
each component of a test -- pre, post, run, out, etc.

DESC test description.
NUMDISKS the number of disks to create, at least two.
RAID raid mode as string (0, 1, or 1v)
FS-STATE a list describing the filesystem state
OP the workload to running following filesystem initialization.
POST-STATE the expected state of the filesystem after OP.
POST-EXTRA-BLOCKS manually add extra dentry blocks not cleaned by rm
OUTPUT the expected output. Generally \"Correct\" or an error."
  "Test template for file creation tasks."
  (define-test
   desc
   (setup-cmd numdisks raid)
   (teardown-cmd)
   (string-join
    (list
     (fs-state-cmds fs-state "d")
     op
     (umount-cmd "mnt")
     (verify-metadata-cmd post-state post-extra-blocks numdisks))
    " && ")
   output
   "0" rc "")) ; pre-rc should always be 0

(defun n-file-directory (n sz)
  (if (= n 0)
      nil
    (cons (cons (format "file%d" n) sz)
	  (n-file-directory (- n 1) sz))))

(defun n-nested-directory (n sz)
  "Create a file of sz in nested directories n deep."
  (if (= n 0)
      (cons (format "file%d" n) sz)
    (list (n-nested-directory (- n 1) sz))))

(defun filesystem-init-success (desc fs-state raid numdisks)
  "Convenience function to generate success fs init tests.

Uses two disks and raid1. Define a custom test for other configurations.

DESC description of test
FS-STATE the target state of the filesystem, as a list."
  (list desc fs-state raid numdisks "Correct\nCorrect" "0"))

(defun filesystem-init-error (desc fs-state errmsg raid numdisks)
  "Convenience function to generate failed fs init tests.

Uses two disks and raid1. Define a custom test for other configurations.

DESC description of the test
FS-STATE the target state of the filesystem. Probably something invalid.
ERRMSG the expected error message."
  (list desc fs-state raid numdisks errmsg "1"))

(defun filesystem-workload-success
    (desc fs-state workload post-state post-blocks msg raid numdisks)
  "Convenience function to generate fs initialization and workload tests.

DESC description of the test
FS-STATE the initial state of the filesystem
WORKLOAD some operations to run after filesystem is initialized
POST-STATE the expected state of the filesystem after WORKLOAD
POST-BLOCKS additional blocks we should expect (e.g. no cleaning dir blocks)"
  (list desc fs-state workload post-state post-blocks raid numdisks
	msg "0"))

(defun filesystem-workload-error
    (desc fs-state workload post-state post-blocks errmsg raid numdisks)
  (list desc fs-state workload post-state post-blocks raid numdisks
	errmsg "1"))

(defun gen-raid-test-with-fn (fn testlist raidconfigs)
  (apply #'append
	 (mapcar (lambda (config)
	      (mapcar (lambda (test)
			(let ((args (append test config)))
			  (apply fn
				 (cons (format "raid%s -- %s" (car config) (car args))
				       (cdr args)))))
		      testlist))
		 raidconfigs)))

; returns (filesystem-init-success 2 "1" "desc" '(())
(generate-tests
 `(((testcase . ,#'mkfs-test)
    ; desc raid numdisks inodes blocks output pre-rc run-rc
    (configs . (("default fs, two disks" "1" 2 32 224 "Success" "0" "0")
		("large fs, four disks" "1" 4 128 1024 "Success" "0" "0")
		("one disk is too few" "1" 1 32 224 "" "1" "1")
		("default fs, striped" "0" 2 32 224 "Success" "0" "0")
		("round blocks correctly 1" "0" 2 30 220 "Success" "0" "0")
		("round blocks correctly 2" "0" 2 33 225 "Success" "0" "0")
		("bad raid mode" "3" 2 32 224 "" "1" "1")
		("too many blocks requested" "1" 2 1024 1024 "" "255" "1")
		("odd number of disks okay" "0" 5 40 300 "Success" "0" "0"))))

   ((testcase . ,#'filesystem-init)
    (configs . ,(gen-raid-test-with-fn
		 #'filesystem-init-success
		  `(("mkdir: single dir" ,'(()))
    		    ("mkdir: nested dir" ,'(() (())))
		    ("mknod: single file" ,'(("file1" . 0)))
		    ("mknod: multi file" ,'(("file1" . 0) () () ("file2" . 0) (("file3" . 0) ("file4" . 0))))
		    ("mknod: large dir" ,(n-file-directory 18 0))
		    ("write: small file" ,'(("file1" . 300)))
		    ("write: two block file" ,'(("file1" . 600)))
		    ("write: many small files" ,(n-file-directory 20 200))
		    ("write: many nested directories" ,(n-nested-directory 10 1024))
		    ("write: indirect block" ,'(("file1" . 8192))))
		  `(("1" 2) ("0" 3))))) ; raid configs MODE NUMDISKS
                                        ; raid1 config has 2 disks, raid0 config has 3 disks
   ((testcase . ,#'filesystem-init)
    (configs . ,(gen-raid-test-with-fn
		 #'filesystem-init-error
		 `(("mknod: run out of inodes" ,(n-file-directory 32 0)
		    "[Errno 28] No space left on device")
		   ("mknod: file exists error" ,'(("file1" . 0) ("file1" . 0))
		    "[Errno 17] File exists")
		   ("write: file exists error" ,'(("file1" . 512) ("file1" . 0))
		    "[Errno 17] File exists"))
		 `(("1" 2) ("0" 3))))) ; raid configs
   ((testcase . ,#'filesystem-init-and-workload)
    (configs . ,(gen-raid-test-with-fn ;; the last number argument on all these is "block adjustment"
		                       ;; it is how we compensate for never deleting a dir data block
		 #'filesystem-workload-success
		  `(("rm: empty file" ,'(("file1" . 0))
		     "rm mnt/file1" ,'() 1 "Correct\nCorrect")
		    ("rmdir: empty dir" ,'(())
		     "rmdir mnt/d1" ,'() 1 "Correct\nCorrect")
		    ("rm: create previously deleted file with data" ,'(("file1" . 1536))
		     ,(concat "rm mnt/file1"
			      " && "
			      (fs-state-cmds '(("file1" . 1536)) "d"))
		     ,'(("file1" . 1536)) 0 "Correct\nCorrect\nCorrect")
		    ("rm: delete file with indirect block" ,'(("file1" . 8192))
		     "rm mnt/file1" ,'() 1 "Correct\nCorrect")
		    ("readdir" ,(n-file-directory 4 0)
		     ,(concat "./readdir-check.py " "4")
		     ,(n-file-directory 4 0) 0 "Correct\nCorrect\nCorrect")
		    ("readdir: large directory" ,(n-file-directory 28 0)
		     ,(concat "./readdir-check.py " "28")
		     ,(n-file-directory 28 0) 0 "Correct\nCorrect\nCorrect")
		    ("read: small file" ,'() ; start with empty mnt dir
		     "./read-write.py 1 10" ; numfiles numwrites (x 100 bytes)
		     ,'(("file1" . 1000)) 0 "Correct\nCorrect\nCorrect") ; py script creates a 1000 byte file
		    ("read: large file" ,'()
		     "./read-write.py 1 80"
		     ,'(("file1" . 8000)) 0 "Correct\nCorrect\nCorrect")
		    ("interleaved writes and readback" ,'()
		     "./read-write.py 6 80"
		     ,(n-file-directory 6 8000) 0 "Correct\nCorrect\nCorrect"))
		  `(("1" 2) ("0" 3)))))
   ((testcase . ,#'filesystem-init-and-workload)
;;    (desc fs-state op post-state post-extra-blocks raid numdisks output rc)
    (configs . (("raid1 -- mount disks in other order" ,'()
		 ,(format "fusermount -u mnt; ../solution/wfs %s %s %s %s -s mnt"
			 (disk-path "test-disk4") (disk-path "test-disk3")
			 (disk-path "test-disk2") (disk-path "test-disk1"))
		  ,'() 0 "1" 4 "Correct\nCorrect" 0)
		 ("raid0 -- mount disks in other order" ,'()
		  ,(format "fusermount -u mnt; ../solution/wfs %s %s %s -s mnt"
			  (disk-path "test-disk3") (disk-path "test-disk2") (disk-path "test-disk1"))
		  ,'() 0 "0" 3 "Correct\nCorrect" 0)
		 ("raid0 -- mount in other order with readback" ,'()
		  ,(string-join
		    (list "./read-write.py 1 50" ; create a 5000-byte file
			  "cat mnt/file1 > file1.test" ; save the file
			  "fusermount -u mnt" ; unmount
			  (format "../solution/wfs %s %s %s -s mnt"
				  (disk-path "test-disk3") (disk-path "test-disk2") (disk-path "test-disk1"))
			  "diff mnt/file1 file1.test") ; should be identical
		    "; ")
		  ,'(("file1" . 5000)) 0 "0" 3 "Correct\nCorrect\nCorrect" 0)
		 ("raid1v -- readback with a corrupted disk" ,'()
		  ,(string-join
		    (list "./read-write.py 1 10"
			  "cat mnt/file1 > file1.test"
			  "fusermount -u mnt"
			  (format "./corrupt-disk.py --disks %s"
				  (disk-path "test-disk1"))
			  (mount-cmd 3 "mnt")
			  "diff mnt/file1 file1.test")
		    "; ")
		  ,'(("file1" . 1000)) 0 "1v" 3 "Correct\nCorrect\nCorrect" 0))))))
