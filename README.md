# MiniShell
Custom shell to Linux operating systems

Implements features such as:
+ Output redirection.
  * `>>` redirects and appends to specified file. 
  * `>!` redirects and overwrites specified file. 
  * `>` redirects to file if file does not already exist. 
  * `>&` and `|&` redirects stdout and stderr to a file or pipe.
+ `cd` - change directory of the shell
+ `setenv` and `unsetenv`. Sets and removes environment variables
+ `source`. Run a file of shell commands.
+ Running commands in the background by detaching processes. Executed by attaching a `&` to the end of any command.

To run MiniShell, it is safest if you run it inside a SafeRun session. SafeRun is a utility, not written by me, which monitors and limits the number of threads produced, CPU time usage, and wall-clock time usage.
MiniShell also relies on SmartAlloc which is another utility, not written by me, which provides simpler commands for the allocation of memory.
`gcc MiniShell.c SmartAlloc.c -o MiniShell`
`SafeRun -T10000000 -t5000 -p50 ./MiniShell`
