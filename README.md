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
