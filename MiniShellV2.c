#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

// compile & run commands
// gcc MiniShell.c SmartAlloc.c -o MiniShell
// SafeRun -T10000000 -t5000 -p50 ./MiniShell

/* Max length of commands, arguments, filenames */
#define MAX_WORD_LEN 100
#define WORD_FMT "%100s"

/*Prototypes*/
static void sourceFile(char *fileName);

static void cdCmd(FILE *in);

/* One argument in a commandline (or the command itself) */
typedef struct Arg {
    char value[MAX_WORD_LEN + 1];
    struct Arg *next;
} Arg;

/* One full command: executable, arguments and any file redirections. */
typedef struct Command {
    int numArgs;
    Arg *args;
    int cmdpid;
    char inFile[MAX_WORD_LEN + 1];
    char outFile[MAX_WORD_LEN + 1];
    int outFileMode;       // 0 - default value
    //                        1 - create and write, refuse if exists (>)
    //                        2 - append (>>)
    //                        3 - force write (>!)
    //                        4 - redirect stderr (>&)
    //                        5 - pipe stderr (|&)
    struct Command *next;
} Command;

// Job is a linked list of Commands
typedef struct Job {
    Command *head;
    int cmdCount;
    int bg;
    char cmdString[200];
    struct Job *next;
} Job;

static Job *HeadJob;

// Make a new Job, given the head of the Command linked list "cmd"
static Job *NewJob(Command *cmd) {
   Job *rtn = malloc(sizeof(Job));
   rtn->bg = 0;
   rtn->head = cmd;
   rtn->cmdCount = 1;
   rtn->next = NULL;

   return rtn;
}

/* Make a new Arg, containing "str" */
static Arg *NewArg(char *str) {
   Arg *rtn = malloc(sizeof(Arg));
   strncpy(rtn->value, str, MAX_WORD_LEN);
   rtn->next = NULL;

   return rtn;
}

/* Make a new Command, with just the executable "cmd" */
static Command *NewCommand(char *cmd) {
   Command *rtn = malloc(sizeof(Command));

   rtn->numArgs = 1;
   rtn->args = NewArg(cmd);
   rtn->inFile[0] = rtn->outFile[0] = '\0';
   rtn->outFileMode = 0;
   rtn->next = NULL;

   return rtn;
}

/* Delete "cmd" and all its Args. */
static Command *DeleteCommand(Command *cmd) {
   Arg *temp;

   while (cmd->args != NULL) {
      temp = cmd->args;
      cmd->args = temp->next;
      free(temp);
   }

   return cmd->next;
}

// searches the given {job} has a command with the given {pid}
// returns 1/true if found. returns 0/false if not found.
static int CheckJobHasPID(Job *job, int pid) {
   Command *cmd;

   cmd = job->head;
   while (cmd != NULL) {
      if (cmd->cmdpid == pid) {
         return 1;
      }
      cmd = cmd->next;
   }

   return 0;
}

// Function goes through all jobs, checking for provided {pid} and decrements the owner job's command count
// returns 1/true if the owner job's command count has reached 0. returns 0/false if there are still cmds remaining
static int DecJobCmdCount(int pid) {
   Job *job;

   job = HeadJob;
   while (job != NULL) {
      if (CheckJobHasPID(job, pid)) {
         job->cmdCount--;
         return !(job->cmdCount);
      }
      job = job->next;
   }

   return 0;
}

// Delete the Job and all of the contained Commands which has a command with the given {pid}
static void DeleteJobWithPid(int pid) {
   // Two pointers for walking down the linked list
   Job *frontJob = HeadJob;
   Job *prevJob = HeadJob;
   Command *cmds;

   if (CheckJobHasPID(frontJob, pid)) {
      HeadJob = frontJob->next;
      cmds = frontJob->head;
      while (cmds != NULL) {
         cmds = DeleteCommand(cmds);
      }
      return;
   }

   while (!CheckJobHasPID(frontJob, pid)) {
      prevJob = frontJob;
      frontJob = prevJob->next;
   }
   prevJob->next = frontJob->next;
   cmds = frontJob->head;
   while (cmds != NULL) {
      cmds = DeleteCommand(cmds);
   }

   return;
}

static void cdCmd(FILE *in) {
   char fileName[MAX_WORD_LEN + 1];

   fscanf(in, WORD_FMT, fileName);
   if (chdir(fileName) == -1) {
      printf("'%s' is not valid dir.\n", fileName);
   }
}

static void envSet(FILE *in) {
   char name[MAX_WORD_LEN + 1]; // doesn't work with char *name; Abnormal termination via signal segmentation fault
   char val[MAX_WORD_LEN + 1];

   fscanf(in, WORD_FMT, name);
   fscanf(in, WORD_FMT, val);
   if (setenv(name, val, 1) == -1) {
      printf("Fail");
   }
}

static void unEnvSet(FILE *in) {
   char name[MAX_WORD_LEN + 1];
   char *var;

   fscanf(in, WORD_FMT, name);
   var = getenv(name);
   (var != NULL) ? unsetenv(name) : printf("Environmental variable not found: %s\n", name);
}

static void printJobs() {
   Job *job = HeadJob;

   while (job != NULL) {
      printf("%s\n", job->cmdString);
      job = job->next;
   }
}


static int ShellCommand(Command *cmd, FILE *in) {
   char nextWord[MAX_WORD_LEN + 1];

   if (cmd && cmd->args && cmd->args->value) {
      if (!strcmp(cmd->args->value, "cd")) {
         cdCmd(in);
         return 1;
      }
      if (!strcmp(cmd->args->value, "setenv")) {
         envSet(in);
         return 1;
      }
      if (!strcmp(cmd->args->value, "unsetenv")) {
         unEnvSet(in);
         return 1;
      }
      if (!strcmp(cmd->args->value, "source")) {
         fscanf(in, WORD_FMT, nextWord);
         sourceFile(nextWord);
         return 1;
      }
      if (!strcmp(cmd->args->value, "jobs")) {
         printJobs();
         return 1;
      }
   }

   return 0;
}

static void handleOutputRedirect(char word[], Command *cmd, Job *job, FILE *in) {
   if (!strcmp(word, ">")) {
      strcat(job->cmdString, " > ");
      cmd->outFileMode = 1;
   }
   else if (!strcmp(word, ">>")) {
      strcat(job->cmdString, " >> ");
      cmd->outFileMode = 2;
   }
   else if (!strcmp(word, ">!")) {
      strcat(job->cmdString, " >! ");
      cmd->outFileMode = 3;
   }
   else if (!strcmp(word, ">&")) {
      strcat(job->cmdString, " >& ");
      cmd->outFileMode = 4;
   }
   fscanf(in, WORD_FMT, cmd->outFile);
   strcat(job->cmdString, cmd->outFile);
}

static void catchCommands(Job *job, int currCmdCount) {
   int tempPID;

   if (job->bg == 0) {  // foreground job
      while (currCmdCount) {  // foreground job's command count
         tempPID = wait(NULL);   // pid found by the wait
         if (CheckJobHasPID(job, tempPID)) { // foreground job's command
            if (DecJobCmdCount(tempPID)) {   // decrement whatever job which has this pid 's command count
               DeleteJobWithPid(tempPID);    // if the job has hit command count = 0, delete that job
            }
            currCmdCount--;
         }
         else {
            if (DecJobCmdCount(tempPID)) {   // decrement whatever job which has this pid 's command count
               DeleteJobWithPid(tempPID);    // if the job has hit command count = 0, delete that job
            }
         }
      }
   }
}

/* Read from "in" a single commandline, comprising one more pipe-connected
   commands.  Return head pointer to the resultant list of Commmands */
static Job *ReadCommands(FILE *in) {
   int nextChar;
   char nextWord[MAX_WORD_LEN + 1];
   Job *job;
   Command *lastCmd;
   Arg *lastArg;

   /* If there is an executable, create a Command for it, else return NULL. */
   if (1 == fscanf(in, WORD_FMT, nextWord)) {
      lastCmd = NewCommand(nextWord);
      lastArg = lastCmd->args;
   }
   else
      return NULL;


   // Checks for cd setenv unsetenv source jobs
   // returns 1 if it was executed, 0 if not a shell command
   if (ShellCommand(lastCmd, in) == 1) {
      return NULL;
   }

   job = NewJob(lastCmd);
   strcat(job->cmdString, nextWord);
   job->next = HeadJob;
   HeadJob = job;

   /* Repeatedly process the next blank delimited string */
   do {
      while ((nextChar = getc(in)) == ' ')   /* Skip whitespace */
         ;

      /* If the line is not over */
      if (nextChar != '\n' && nextChar != EOF) {

         /* A pipe indicates a new command */
         if (nextChar == '|') {

            if ((nextChar = getc(in)) == '&') { // stderr redirect
               lastCmd->outFileMode = 5;
               strcat(job->cmdString, "|&");
            }
            else {
               ungetc(nextChar, in);
            }

            if (1 == fscanf(in, WORD_FMT, nextWord)) {
               strcat(job->cmdString, " | ");
               strcat(job->cmdString, nextWord);
               job->cmdCount++;
               lastCmd = lastCmd->next = NewCommand(nextWord);
               lastArg = lastCmd->args;
            }
         }
         else if (nextChar == '&') { // background the process
            job->bg = 1;
            strcat(job->cmdString, " &");
         }

            /* Otherwise, it's either a redirect, or a commandline arg */
         else {
            ungetc(nextChar, in);
            fscanf(in, WORD_FMT, nextWord);
            if (!strcmp(nextWord, "<")) {
               fscanf(in, WORD_FMT, lastCmd->inFile);
               strcat(job->cmdString, " < ");
               strcat(job->cmdString, lastCmd->inFile);
            }
            else if (!strcmp(nextWord, ">") || !strcmp(nextWord, ">>") ||
                       !strcmp(nextWord, ">!") || !strcmp(nextWord, ">&")) {
               handleOutputRedirect(nextWord, lastCmd, job, in);
            }
            else {
               lastArg = lastArg->next = NewArg(nextWord);
               strcat(job->cmdString, " ");
               strcat(job->cmdString, nextWord);
               lastCmd->numArgs++;
            }
         }
      }
   } while (nextChar != '\n' && nextChar != EOF);

   return job;
}

static void RunCommands(Job *job) {
   Command *cmd;
   char **cmdArgs, **thisArg;
   Arg *arg;
   int childPID, currJobCmdCount = 0;
   int pipeFDs[2]; /* Pipe fds for pipe between this command and the next */
   int inFD = -1, outFD; /* If not -1, FD of pipe or file to use for stdin stdout respectively */

   for (cmd = job->head; cmd != NULL; cmd = cmd->next) {
      if (inFD < 0 && cmd->inFile[0]) /* If no in-pipe, but input redirect */
         inFD = open(cmd->inFile, O_RDONLY);

      if (cmd->next != NULL)  /* If there's a next command, make an out-pipe */
         pipe(pipeFDs);

      if ((childPID = fork()) < 0) {
         fprintf(stderr, "Error, cannot fork.\n");
         perror("");
      }
      else if (childPID) {      /* We are parent */
         cmd->cmdpid = childPID;
         currJobCmdCount++;
         close(inFD);           /* Parent doesn't use inFd; child does */
         if (cmd->next != NULL) {
            close(pipeFDs[1]);  /* Parent doesn't use out-pipe; child does */
            inFD = pipeFDs[0];  /* Next child's inFD will be out-pipe reader */
         }
      }
      else {                         /* We are child */
         if (inFD >= 0) {            /* If special input fd is set up ...  */
            dup2(inFD, 0);           /*   Move it to fd 0 */
            close(inFD);             /*   Close original fd in favor of fd 0 */
         }

         outFD = -1;                 /* Set up special stdout, if any */
         if (cmd->next != NULL) {    /* if our parent arranged an out-pipe.. */
            outFD = pipeFDs[1];      /*   Save its write fd as our outFD */
            close(pipeFDs[0]);       /*   Close read fd; next cmd will read */
         }
         if (outFD < 0 && cmd->outFile[0] && cmd->outFileMode == 1)  /* If no out-pipe, but a redirect > */
         {
            if (access(cmd->outFile, F_OK) != -1) {   // file exists
               printf("Redirection would overwrite output\n");
               break;
            }
            else {    // file doesn't exist
               outFD = open(cmd->outFile, O_WRONLY | O_CREAT | O_EXCL, 0644);
            }
         }
         if (outFD < 0 && cmd->outFile[0] && cmd->outFileMode == 2)  /* If no out-pipe, but a redirect >> */
            outFD = open(cmd->outFile, O_WRONLY | O_CREAT | O_APPEND, 0644);
         if (outFD < 0 && cmd->outFile[0] && cmd->outFileMode == 3)  /* If no out-pipe, but a redirect >! */
            outFD = open(cmd->outFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
         if (outFD < 0 && cmd->outFile[0] && cmd->outFileMode == 4) { /* If no out-pipe, but a redirect >& */
            outFD = open(cmd->outFile, O_WRONLY | O_CREAT | O_APPEND, 0644);
         }
         /* If above code results in special stdout, dup this to fd 1 */

         if (outFD >= 0) {
            dup2(outFD, 1);
            if (cmd->outFileMode >= 4) {
               dup2(outFD, 2);
            }
            close(outFD);
         }

         /* Build a commandline arguments array, and point it to Args content */
         cmdArgs = thisArg = calloc(sizeof(char *), (size_t) (cmd->numArgs + 1));
         for (arg = cmd->args; arg != NULL; arg = arg->next) {
            *thisArg++ = arg->value;
         }

         /* Exec the command, with given args, and with stdin, stdout, stderr
            remapped if we did so above */
         execvp(*cmdArgs, cmdArgs);
      }
   }
   catchCommands(job, currJobCmdCount);
}


static void sourceFile(char *fileName) {
   Command *cmd;
   Job *job;
   FILE *fp;

   if (!(fp = fopen(fileName, "r")))
      perror("");
   else {
      while (!feof(fp)) {
         if ((job = ReadCommands(fp)) != NULL) {
            if ((job->head) != NULL) {
               RunCommands(job);
            }
         }

         if (job != NULL && !job->bg) {
            cmd = job->head;
            while (cmd != NULL) {
               cmd = DeleteCommand(cmd);
            }
         }
      }
   }
}

int main() {
   Command *cmd;
   Job *job;

   /* Repeatedly print a prompt, read a commandline, run it, and delete it */
   while (!feof(stdin)) {
      printf(">> ");

      if ((job = ReadCommands(stdin)) != NULL) {
         if ((cmd = job->head) != NULL) {
            RunCommands(job);
         }
      }
      if (job != NULL && !job->bg) {
         cmd = job->head;
         while (cmd != NULL) {
            cmd = DeleteCommand(cmd);
         }
      }
   }
}