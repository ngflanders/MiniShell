#define _GNU_SOURCE
#define _XOPEN_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>
#include <string.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>
#include <sched.h>
#include <limits.h>
#include <dirent.h>

// IHS-culpable failures
const int IHS_ERROR = 180;
const int BAD_FORK = 180;
const int SETUP_ERR = 181;
const int BAD_WAIT = 182;
const int BAD_CHROOT = 183;
const int BAD_EXEC = 184;
const int BAD_USER = 185;
const int BAD_SELECT = 186;
const int INTERRUPTED = 187;

// App-culpable failures -- or-ed plus 192
const int APP_FAILURE_BASE = 0xC0;
const int TIMEOUT = 0x1;
const int OUTPUT_OVERRUN = 0x2;
const int ROGUE_PROCS = 0x4;
const int RT_FAULT = 0x8;
const int UNREAD_INPUT = 0x10;
const int RESERVED = 0x20;

#define BUF_SIZE 4096
#define PTY_NAME_LEN 128
#define MS_CHECK_TIME 20
#define USR_NAME_LEN 20
#define MAX_MOUNTS 16
#define MAX_PROCS 100
#define LOCKFILE_NAME "/var/lock/SafeRun.%s.lock"
#define LOCKFILE_NAME_LEN 23

// Parametric data for input pumping thread
typedef struct {
   sem_t EOFSem;
   int outFd;
   int isPty;
   int go;
} InputParams;

// Parametric data for output pumping thread
typedef struct {
   int outFd;     // fd to send data to generally 1 or 2
   int inFd;      // fd of pty from which data comes 
   int byteLimit; // -1 for unlimited
   int status;
   int bytesRead;
   FILE *reportStr; // Stream on which to report errors
} OutputParams;

typedef struct {
   int maxOutput;             // What is the largest output size in bytes?
   int allowFiles;            // Do we allow creation of files?
   int maxProcs;              // At least 1
   int maxMsCPU;              // CPU timelimit
   int maxMsRuntime;          // Wallclock runtime limit
   int maxMem;                // Address space limit in bytes
   int unreadInputAllowed;    // Exact number of unread input bytes, or -1 for any number
   int binaryInput;           // Transfer input via pipe (for binary data)
   int stdoutErrors;          // Put error messages into stdout stream
   int chrootJail;            // Jail in "." dir

   // CJF - Software Inventions, Inc
   int unshareNetwork;        // Disable networking
   char user[USR_NAME_LEN+1]; // User to use for subprocess control
   int userMutex;             // Create and honor per-user mutexes
   char mount[MAX_MOUNTS+1][PATH_MAX+1]; // Paths to bind-mount inside the jail,
                                         // terminated by empty string
   int quiet;                 // Suppress error messages
} RunLimits;

static int interrupted = 0;
static void setBreak(int sig) {
   interrupted = 1;
}

void Report(FILE *reportStr, char *format, int prm) {
   if (reportStr)
      fprintf(reportStr, format, prm);
}

// Set up pty for input so that it doesn't echo, etc.
void ConditionTerminal(int fd) {
   struct termios status;

   tcgetattr(fd, &status);
   status.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHONL);
   status.c_oflag &= ~ONLCR;
   tcsetattr(fd, TCSANOW, &status);
}

void *PumpInput(void *params) {
   InputParams *ip = (InputParams *)params;
   char inBuf[BUF_SIZE], eofChr = '\04';
   struct timespec eofWait = {0, 100000000};      // Wait .1s between EOF redos
   struct pollfd pfd = {ip->outFd, POLLOUT, 0}; 
   int ready, charsRead = 0, atStart = 1;
   fd_set rfds;
   struct timeval tv;

   do {
      FD_ZERO(&rfds);
      FD_SET(0, &rfds);
      tv.tv_sec = 0;
      tv.tv_usec = 50000;
      if (1 == (ready = select(1, &rfds, NULL, NULL, &tv))) {
         charsRead = read(0, inBuf, BUF_SIZE);
         if (charsRead) {
            atStart = inBuf[charsRead-1] == '\n';
            write(ip->outFd, inBuf, charsRead);
         }
      }
   } while (ip->go && (!ready || charsRead > 0));

   if (ip->isPty) {
      // If we hit EOF, imitate EOF by writing ^D.  Note that per Unix
      // docs, an EOF is a transparent EOL marker, not a true EOF marker.
      // Thus each reader requires their own ^D, forcing a 0 length read 
      // if currently at start of line.  And this must be preceded by a ^D
      // to transmit any currently incomplete line, if it exists.  Finally,
      // any process that attempts to read past EOF repeatedly on a pty
      // requires a ^D for each such attempt, or it will hang!
      if (charsRead == 0) {
         if (!atStart) { // Clear input line with a prelim ^D
            write(ip->outFd, &eofChr, 1);
         }

         // Keep sending eofChr if the main doesn't reach mopup point, since
         // subject process may be reading past EOF multiple times. But wait
         // a bit between each eofChr, just to avoid excess traffic. And write
         // only if there is room 
         while (-1 == sem_trywait(&ip->EOFSem)) {
            if (poll(&pfd, 1, 0) > 0)
               write(ip->outFd, &eofChr, 1);
            nanosleep(&eofWait, 0);
         }
      }
      else
         sem_wait(&ip->EOFSem); // No EOF if we we stopped with unusued chars.

      write(ip->outFd, &eofChr, 1); // One more for the mopUp call
   }
   close(ip->outFd);

   return NULL;
}

void *PumpOutput(void *params) {
   OutputParams *op = (OutputParams *)params;
   char outBuf[BUF_SIZE];
   int charsRead, toWrite, bytesAllowed = op->byteLimit;

   op->bytesRead = 0;
   while (0 < (charsRead = read(op->inFd, outBuf, BUF_SIZE))) {
      toWrite = bytesAllowed < charsRead ? bytesAllowed : charsRead;
      if (toWrite > 0) {
         write(op->outFd, outBuf, toWrite);
         bytesAllowed -= toWrite;
      }
      op->bytesRead += charsRead;
   }

   if (op->bytesRead > op->byteLimit) {
      sprintf(outBuf, "... and %d dropped bytes\n", op->bytesRead - op->byteLimit);
      write(op->outFd, outBuf, strlen(outBuf));
   }

   op->status = op->bytesRead > op->byteLimit ? OUTPUT_OVERRUN : 0;
   return NULL;
}

void ProcessArgs(char ***argv, RunLimits *lims) {
   char flag;
   int mountNdx = 0;

   while (*++*argv && ***argv == '-') {
      flag = *++**argv;
      if (flag == 'p') {                         // Process limit
         lims->maxProcs = atoi(++**argv);
         lims->maxProcs = lims->maxProcs < 1 ? 1 : 
         lims->maxProcs > MAX_PROCS ? MAX_PROCS : lims->maxProcs;
      }
      else if (flag == 'o')                    // Output limit
         lims->maxOutput = atoi(++**argv);
      else if (flag == 'T')                    // Wallclock time
         lims->maxMsRuntime = atoi(++**argv);
      else if (flag == 't')                    // CPU mS
         lims->maxMsCPU = atoi(++**argv);
      else if (flag == 's')                    // Virtual memory bytes
         lims->maxMem = atoi(++**argv);
      else if (flag == 'f')                    // Number of open files
         lims->allowFiles = atoi(++**argv) > 0;// For now...
      else if (flag == 'r')                    // Do chroot jail
         lims->chrootJail = 1;
      else if (flag == 'n')                    // Unshare network namespace
         lims->unshareNetwork = 1;
      else if (flag == 'u') {
         *lims->user = '\0';
         strncat(lims->user, ++**argv, USR_NAME_LEN);
      }
      else if (flag == 'd' && mountNdx < MAX_MOUNTS) {
         *lims->mount[mountNdx] = '\0';
         strncat(lims->mount[mountNdx++], ++**argv, PATH_MAX);
      }
      else if (flag == 'i') {
         if (*++**argv)
            lims->unreadInputAllowed = atoi(**argv);
         else
            lims->unreadInputAllowed = -1;
      }
      else if (flag == 'b')
         lims->binaryInput = 1;
      else if (flag == 'm')
         lims->stdoutErrors = 1;
      else if (flag == 'q')
         lims->quiet = 1;
      else if (flag == 'x')
         lims->userMutex = 1;
   }

   *lims->mount[mountNdx] = '\0';

   if (!**argv)
      printf("Usage: SafeRun <opts> command\n");
}

int OpenMasterPty(int *fd, char **sName) {
   return 0 <= (*fd = getpt()) && !grantpt(*fd)
    && NULL != (*sName = ptsname(*fd))
    && !unlockpt(*fd);

}

// CJF - Software Inventions, Inc
// Kill the test subject and any other processes it might have spawned.
// Always kill by process group id, but as this can be easily evaded, also kill
// by user if possible. First, atomically lower the scheduling priority of the
// user's processes to gain an advantage in race conditions. Then, spawn a
// process as the user to kill all other processes.
int FindRogues(int ssid, char *user) {
   int rogues = 0, pid;
   struct passwd *pwd;

   if (*user && (pwd = getpwnam(user))
    && !setpriority(PRIO_USER, pwd->pw_uid, 19)) {
      if (!(pid = fork())) {
         if (!setuid(pwd->pw_uid))
            exit(!kill(-1, 9));
         fprintf(stderr, "setuid for kill failed: %s\n", strerror(errno));
         exit(-1);
      }
      else if (pid > 0) {
         waitpid(pid, &rogues, 0);
         rogues = WIFEXITED(rogues) && WEXITSTATUS(rogues);
      }
      else
         fprintf(stderr, "fork failed: %s\n", strerror(errno));
   }

   if (ssid)
      rogues |= !kill(-ssid, 9);

   return rogues;
}

void MopUpInput(int fd, int byteLimit, int *progErrors, FILE *reportStr) { 
   char extraInBuf[BUF_SIZE];
   int extraIn, extraInBytes = 0; 

   do {
      extraIn = 0;
      if ((extraIn = read(fd, extraInBuf, BUF_SIZE)) > 0)
         extraInBytes += extraIn;
   } while (extraIn > 0);
      
   close(fd);

   if (extraInBytes && byteLimit != -1 && byteLimit != extraInBytes) {
      Report(reportStr, "%d input bytes dropped\n", extraInBytes);
      *progErrors |= UNREAD_INPUT;
   }
}

// CJF - Software Inventions, Inc
// Ensure that |uid| has ownership of, and read/write permission to, |path|
// and all contents, recursively (like `chown -R` and `chmod -R`).
// TODO could test subjects create deeply-nested directories and cause a stack
// overflow here?
int ChownContents(char *path, uid_t uid) {
   struct stat stat;
   DIR *dirp;
   struct dirent *ent;
   char sub_path[PATH_MAX+1];

   if (lchown(path, uid, -1)
    || lstat(path, &stat)
    || chmod(path, stat.st_mode | S_IRUSR | S_IWUSR))
      return -1;
   if (S_ISDIR(stat.st_mode)) {
      if ((dirp = opendir(path)) == NULL)
         return -1;
      while ((ent = readdir(dirp))) {
         if (ent->d_name[0] == '.'
          && (!ent->d_name[1] || ent->d_name[1] == '.'))
            continue;
         if (snprintf(sub_path, PATH_MAX+1, "%s/%s", path, ent->d_name) < 0
          || ChownContents(sub_path, uid))
            return -1;
      }
      closedir(dirp);
   }
   return 0;
}

// CJF - Software Inventions, Inc
// Ensure that the directory |path| exists, creating intermediate
// directories if necessary (like `mkdir -p`).
int MakePath(char *path, mode_t mode) {
   char *p;

   for (p = strchr(path, '/'); p; p = strchr(p+1, '/')) {
      *p = '\0';
      if (mkdir(path, mode) && errno != EEXIST) {
         *p = '/';
         return -1;
      }
      *p = '/';
   }
   if (mkdir(path, mode) && errno != EEXIST)
      return -1;
   return 0;
}

int DoChild(RunLimits limits, int inPipe[], int errPipe[], int mPty, int sPty,
 char **argv) {
   struct rlimit procLim, memLim, cpuLim;
   struct passwd *pwd;
   char (*source_path)[PATH_MAX+1];
   char dest_path[PATH_MAX+1];
   int unshare_flags;

   close(mPty);

   if (*limits.user) {
      pwd = getpwnam(limits.user);  // Before we jail ourselves
      if (!pwd || ChownContents(".", pwd->pw_uid))
         return BAD_USER;
   }

   // CJF - Software Inventions, Inc
   // Create new Linux namespaces, bind-mount each entry in |limits.mount|
   // within the current directory, and chroot into the current directory.
   // References for unshare and clone:
   // a. search for "Linux namespaces" and there are good descriptions
   // b. Experiment with unshare(1), e.g.`sudo unshare -n ifconfig -a`
   // c. Sandstorm.io uses namespaces for application sandboxing and has
   //    well-commented code: http://git.io/GUKf_Q
   if (limits.chrootJail) {
      unshare_flags = CLONE_NEWNS | CLONE_NEWIPC | CLONE_NEWUTS;
      if (limits.unshareNetwork)
         unshare_flags |= CLONE_NEWNET;
      if (unshare(unshare_flags))
         return BAD_CHROOT;

      for (source_path = limits.mount; **source_path; source_path++) {
         *dest_path = '\0';
         strncat(dest_path, *source_path + 1, PATH_MAX);
         if (MakePath(dest_path, 0755)
          || mount(*source_path, dest_path, NULL, MS_BIND, NULL))
            return BAD_CHROOT;
      }

      if (chroot("."))
         return BAD_CHROOT;
   }

   if (*limits.user) {
      if (setuid(pwd->pw_uid))
         return BAD_USER;
   }
   setsid();  // Child is in new session.

   // Limit child resources per |limits| values, and to block process runaways
   cpuLim.rlim_cur = cpuLim.rlim_max = (limits.maxMsCPU+999)/1000;
   memLim.rlim_cur = memLim.rlim_max = limits.maxMem;
   procLim.rlim_cur = procLim.rlim_max = limits.maxProcs;
   if (setrlimit(RLIMIT_CPU, &cpuLim)
    || setrlimit(RLIMIT_AS, &memLim)
    || setrlimit(RLIMIT_NPROC, &procLim)) {
      fprintf(stderr, "setrlimit failed: %s\n", strerror(errno));
      return BAD_USER;
   }

   if (limits.binaryInput) {
      dup2(inPipe[0], STDIN_FILENO);
      close(inPipe[0]);
      close(inPipe[1]);
   }
   else
      dup2(sPty, STDIN_FILENO);

   dup2(sPty, STDOUT_FILENO);

   dup2(errPipe[1], STDERR_FILENO);
   close(errPipe[1]);
   close(errPipe[0]);
   close(sPty);

   execvp(*argv, argv);
   fprintf(stderr, "Exec failed: %s\n", strerror(errno));
   return BAD_EXEC;
}

int GetRealMs(struct rusage *ru) {
   return (ru->ru_utime.tv_sec + ru->ru_stime.tv_sec)*1000 
    + (ru->ru_utime.tv_usec + ru->ru_stime.tv_usec)/1000;
}

int ShowCode(FILE *reportStr, int IHSError, int progErrors, int status) {
   if (IHSError) {
      Report(reportStr, "SafeRun problem: error code %d\n", IHSError);
      return IHSError;
   }
   else if (progErrors) {
      return APP_FAILURE_BASE + progErrors;
   }
   else if (status)
      Report(reportStr, "Your program exited with nonzero code %d\n", status);

   return status;
}

int main(int argc, char **argv, char **envp) {
   // Default limits: 1M output, no files, 10 procs, 2s CPU, 10s runtime,
   // 2GB address space, no unread input allowed, test subject errs to stderr,
   // no chroot jail, no user, no mutex, no mount dirs, no quiet
   RunLimits limits = {1000000, 0, 10, 2000, 10000, 2000000000, 0, 0, 0, 0, 0,
    "", 0, {""}, 0};
   int elapsedUs, child, mPty, sPty, lockFd, mopUpFd, progErrors = 0, wResult;
   int numRogues, msUsage, IHSError = 0, status = 0;
   char *slaveName, lockfileName[LOCKFILE_NAME_LEN + USR_NAME_LEN + 1] = "";
   pthread_t inThread, outThread, errThread;
   InputParams inPrms;
   OutputParams outPrms, errPrms;
   int errPipe[2], inPipe[2];  // Simple pipe for stderr, and maybe stdin
   struct rusage selfUsage, childUsage;
   FILE *reportStr;

   ProcessArgs(&argv, &limits);
   reportStr = limits.quiet ? NULL : limits.stdoutErrors ? stdout : stderr;
   
   if (!OpenMasterPty(&mPty, &slaveName) || pipe(errPipe) 
    || limits.binaryInput && pipe(inPipe))
      return SETUP_ERR;

   ConditionTerminal(mPty);
   sPty = open(slaveName, limits.binaryInput ? O_WRONLY : O_RDWR);

   // CJF - Software Inventions, Inc
   if (*limits.user) {
      FindRogues(0, limits.user); // Clean up any processes from earlier runs
      if (limits.userMutex) {
         sprintf(lockfileName, LOCKFILE_NAME, limits.user);
         lockFd = open(lockfileName, O_RDONLY | O_CREAT | O_CLOEXEC, 0444);
         if (lockFd < 0 || flock(lockFd, LOCK_EX | LOCK_NB)) {
            fprintf(reportStr, "User already in use\n");
            return BAD_USER;
         }
      }
   }
   else if (geteuid() == 0) {
      fprintf(reportStr, "User required when running as root\n");
      return BAD_USER; // Refuse to run child as root.
   }

   if ((child = fork()) < 0) {
      return BAD_FORK;
   }
   else if (!child)
      return DoChild(limits, inPipe, errPipe, mPty, sPty, argv);
   else {            // Parent logic
      signal(SIGINT, setBreak);
      signal(SIGQUIT, setBreak);
      signal(SIGTERM, setBreak);
      
      sem_init(&inPrms.EOFSem, 0, 0);
      if (inPrms.isPty = !limits.binaryInput) {
         inPrms.outFd = mPty;
         mopUpFd = sPty;
      }
      else {
         inPrms.outFd = inPipe[1];
         mopUpFd = inPipe[0];
         close(sPty);
      }
      inPrms.go = 1;

      outPrms.inFd = mPty;
      outPrms.byteLimit = errPrms.byteLimit = limits.maxOutput;
      outPrms.outFd = 1;
      outPrms.reportStr = reportStr;

      errPrms.inFd = errPipe[0];
      close(errPipe[1]);
      errPrms.outFd = 2;
      
      pthread_create(&inThread, NULL, PumpInput, &inPrms);
      pthread_create(&outThread, NULL, PumpOutput, &outPrms);
      pthread_create(&errThread, NULL, PumpOutput, &errPrms);

      for (elapsedUs = 0; elapsedUs < limits.maxMsRuntime && !interrupted &&
       !(wResult = waitpid(child, &status, WNOHANG));
       elapsedUs += MS_CHECK_TIME) {
         usleep(1000*MS_CHECK_TIME);
      }

      // If the wait loop exceeded max time, kill the child anyway
      if (wResult != child) {
         kill(child, SIGKILL);
         wResult = waitpid(child, &status, 0);
         Report(reportStr, "Wallclock time exceeded %d mS\n",
          limits.maxMsRuntime);
         progErrors |= TIMEOUT;
      }

      if (interrupted)
         IHSError = INTERRUPTED;
      else if (wResult && wResult != child)
         IHSError = BAD_WAIT;
      else if (!WIFEXITED(status)) {
         if (reportStr)
            if (WIFSIGNALED(status))
               fprintf(reportStr, "Abnormal termination via signal %s\n",
                strsignal(WTERMSIG(status)));
            else
               fprintf(reportStr, "Abnormal termination\n");
         progErrors |= RT_FAULT;
      }         
      else {
         status = WEXITSTATUS(status);
         // Ensure status is less than APP_FAILURE_BASE to avoid subsequent
         // confusion, and save any child-reported error code
         if (status >= APP_FAILURE_BASE)
            status = IHS_ERROR - 1;
         else if (status >= IHS_ERROR)
            IHSError = status;
      }
         
      if (numRogues = FindRogues(child, limits.user)) {
         Report(reportStr, "Killed %d rogue child processes\n", numRogues);
         progErrors |= ROGUE_PROCS;
      }

      getrusage(RUSAGE_CHILDREN, &childUsage);
      getrusage(RUSAGE_SELF, &selfUsage);


      msUsage = GetRealMs(&selfUsage) + GetRealMs(&childUsage);

      if (limits.maxMsCPU <= msUsage) {
         Report(reportStr, "CPU time exceeded %d mS\n", limits.maxMsCPU);
         progErrors |= TIMEOUT;
      }

      // Stop the input thread if we're not going to wait for a formal EOF
      if (limits.unreadInputAllowed == -1)
         inPrms.go = 0;

      // Tell PumpInput to stop sending EOFs to child, but do one 
      // more EOF for MopUpInput
      sem_post(&inPrms.EOFSem);
      MopUpInput(mopUpFd, limits.unreadInputAllowed, &progErrors, reportStr);

      pthread_join(inThread, NULL);
      pthread_join(outThread, NULL);
      pthread_join(errThread, NULL);
      close(mPty);
      close(errPipe[0]);

      // Give access back to the calling user, if we were setuid root
      if (*limits.user) {
         ChownContents(".", getuid());
         if (limits.userMutex)
            unlink(lockfileName);
      }
   }
   
   return ShowCode(reportStr, IHSError, progErrors, status);
}
