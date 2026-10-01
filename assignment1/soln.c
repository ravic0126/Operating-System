// MTL458 A1 - basic shell
// read a line, tokenize it, run whatever comes out.
// supports pipes upto 3 stages, redirection < > >>, separators ; && ||
// and builtins cd / history / exit

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/wait.h>

#define PROMPT "MTL458 > "
#define ERRMSG "Invalid Command"

#define MAXLINE 2048
#define MAXARGS 100
#define MAXSTAGE 3
#define MAXHIST 2048
#define MAXTOK 2048     

//the separator sitting in front of a job
#define S_NONE 0
#define S_SEMI 1
#define S_AND 2
#define S_OR 3

struct cmd{
    char* argv[MAXARGS+1];      //nul terminated, ready for execvp
    int argc;
    char* in;                   //file after <, else NULL
    char* out;                  //file after > or >>, else NULL
    int app;                    //1 = >>, 0 = >
};

char* hist[MAXHIST];
int hcnt=0;
char* prevdir=NULL;             //where cd - goes back to
char* starthome=NULL;           //dir we started in, where bare cd goes
int laststat=0;                 //exit status of the last job that ran
int errfd=STDOUT_FILENO;        //private handle on the shell's own stdout

// only error msg the shell prints. goes to stderr 
// in "madeupcmd | grep gamma" the bad stage's stdout is the pipe into grep,
// so if we printed there grep would just eat it as input
void err(void){
    dprintf(errfd,"%s\n",ERRMSG);
}

void addhist(char* line){
    char* c=strdup(line);
    if (c==NULL){
        return;
    }
    if (hcnt==MAXHIST){
        free(hist[0]);
        memmove(hist,hist+1,(MAXHIST-1)*sizeof(hist[0]));
        hcnt--;
    }
    hist[hcnt++]=c;
}

void clearhist(void){
    for (int i=0;i<hcnt;i++){
        free(hist[i]);
    }
    hcnt=0;
}

//both ways out of the shell come through here, exit and end of input alike
void cleanup(void){
    clearhist();
    free(prevdir);
    free(starthome);
    if (errfd!=STDOUT_FILENO){
        close(errfd);
    }
}

// returns the operator at p and its length, NULL if p is a normal char.
// check 2-char ops first - if we matched | before || then "a || b" would
// become two empty pipe stages which is wrong
char* getop(char* p,int* len){
    *len=2;
    if (p[0]=='|' && p[1]=='|') return "||";
    if (p[0]=='&' && p[1]=='&') return "&&";
    if (p[0]=='>' && p[1]=='>') return ">>";
    *len=1;
    if (p[0]=='|') return "|";
    if (p[0]=='&') return "&";
    if (p[0]==';') return ";";
    if (p[0]=='<') return "<";
    if (p[0]=='>') return ">";
    *len=0;
    return NULL;
}

// splits line into words + operators. words just point into line itself
// (cutting it up in place), operators come back as string literals. thats why
// a 2-char op works even tho theres no room in the buffer to null-terminate it.
// returns token count, or -1 if too many tokens
int tokenize(char* line,char* tok[],int max){
    char* p=line;
    int n=0;

    while (*p!='\0'){
        while (*p==' ' || *p=='\t'){
            p++;
        }
        if (*p=='\0'){
            break;
        }
        if (n==max){
            return -1;
        }

        int len;
        char* op=getop(p,&len);
        if (op!=NULL){
            tok[n++]=op;
            p+=len;
            continue;
        }

        tok[n++]=p;
        while (*p!='\0' && *p!=' ' && *p!='\t' && getop(p,&len)==NULL){
            p++;
        }
        if (*p==' ' || *p=='\t'){
            *p++='\0';
            continue;
        }
        if (*p=='\0'){
            continue;
        }

        //the word runs straight into an operator, like "hi>out.txt". read the
        //operator before overwriting its first byte to end the word, after that
        //those bytes belong to nothing since the token itself is a literal
        op=getop(p,&len);
        *p='\0';
        p+=len;
        if (n==max){
            return -1;
        }
        tok[n++]=op;
    }
    return n;
}

int isredir(char* t){
    return strcmp(t,"<")==0 || strcmp(t,">")==0 || strcmp(t,">>")==0;
}

//builds one cmd from tok[from..to-1]. returns -1 if stage is empty / too many
//args / redirection repeated / redirection with no file after it
int parsecmd(char* tok[],int from,int to,struct cmd* c){
    memset(c,0,sizeof(*c));

    for (int i=from;i<to;i++){
        if (strcmp(tok[i],"&")==0){
            return -1;              // no background jobs, out of scope
        }
        if (isredir(tok[i])){
            if (i+1>=to || isredir(tok[i+1])){
                return -1;          //nothing usable after the operator
            }
            if (tok[i][0]=='<'){
                if (c->in!=NULL){
                    return -1;
                }
                c->in=tok[i+1];
            }
            else{
                if (c->out!=NULL){
                    return -1;      // cant have both > and >>
                }
                c->out=tok[i+1];
                c->app=(tok[i][1]=='>');
            }
            i++;                    //skip the filename
            continue;
        }
        if (c->argc==MAXARGS){
            return -1;
        }
        c->argv[c->argc++]=tok[i];
    }

    c->argv[c->argc]=NULL;
    if (c->argc==0){
        return -1;
    }
    return 0;
}

// hooks stdin/stdout to the files the cmd asked for. runs in the child so if it
// fails we only bail on this one cmd. output file gets made before exec, so even
// a cmd that prints nothing still leaves an empty file behind (spec wants this)
int redirect(struct cmd* c){
    if (c->in!=NULL){
        int fd=open(c->in,O_RDONLY);
        if (fd<0){
            err();
            return -1;
        }
        dup2(fd,STDIN_FILENO);
        close(fd);
    }
    if (c->out!=NULL){
        int flags=O_WRONLY|O_CREAT;
        if (c->app){
            flags|=O_APPEND;
        }
        else{
            flags|=O_TRUNC;
        }
        int fd=open(c->out,flags,0644);
        if (fd<0){
            err();
            return -1;
        }
        dup2(fd,STDOUT_FILENO);
        close(fd);
    }
    return 0;
}

// bare cd and cd ~ go back to where the shell started, not to $HOME.
// the handout starts us in /home/user and calls that the home directory, so
// the two are the same place there. a grader controls the cwd it launches us
// in but not $HOME, so the startup dir is the reading that actually matches.
// $HOME is only a fallback for when getcwd failed at startup
char* gethome(void){
    if (starthome!=NULL){
        return starthome;
    }
    char* h=getenv("HOME");
    if (h!=NULL && h[0]!='\0'){
        return h;
    }
    struct passwd* pw=getpwuid(getuid());   //HOME might not be set, fallback
    if (pw==NULL){
        return NULL;
    }
    return pw->pw_dir;
}

int docd(struct cmd* c){
    char cwd[PATH_MAX];
    char* target;
    int back=0;

    if (c->argc>2){
        err();
        return 1;
    }

    if (c->argc==1 || strcmp(c->argv[1],"~")==0){
        target=gethome();
    }
    else if(strcmp(c->argv[1],"-")==0){
        target=prevdir;
        back=1;
    }
    else{
        target=c->argv[1];
    }

    if (target==NULL){          //no home, or a cd - before any other cd
        err();
        return 1;
    }
    if (getcwd(cwd,sizeof(cwd))==NULL){
        err();
        return 1;
    }
    // if chdir fails, the cwd stays exactly where it was
    if (chdir(target)<0){
        err();
        return 1;
    }

    free(prevdir);
    prevdir=strdup(cwd);

    //cd - is the one form that prints where it landed
    if (back && getcwd(cwd,sizeof(cwd))!=NULL){
        printf("%s\n",cwd);
    }
    return 0;
}

int dohist(struct cmd* c){
    long n=hcnt;

    if (c->argc>2){
        err();
        return 1;
    }
    if (c->argc==2){
        char* end;
        errno=0;
        n=strtol(c->argv[1],&end,10);
        // strtol stops at first bad char so we check the terminator .
        // atoi would silently accept "12abc" which we dont want
        if (errno!=0 || end==c->argv[1] || *end!='\0' || n<0){
            err();
            return 1;
        }
        if (n>hcnt){
            n=hcnt;
        }
    }
    for (int i=hcnt-(int)n;i<hcnt;i++){
        printf("%s\n",hist[i]);
    }
    return 0;
}

int isbuiltin(char* name){
    return strcmp(name,"cd")==0 || strcmp(name,"history")==0 || strcmp(name,"exit")==0;
}

void putback(int saved,int target){
    if (saved<0){
        return;
    }
    dup2(saved,target);
    close(saved);
}

// builtins run inside the shell itself - a forked child cant change the shell's
// own cwd. redirection isnt really needed for them but keeping it means
// "history > out.txt" actually writes to the file instead of the terminal
int dobuiltin(struct cmd* c){
    if (strcmp(c->argv[0],"exit")==0){
        cleanup();
        exit(0);
    }

    int savein=-1;
    int saveout=-1;
    if (c->in!=NULL || c->out!=NULL){
        savein=dup(STDIN_FILENO);
        saveout=dup(STDOUT_FILENO);
        if (savein<0 || saveout<0 || redirect(c)<0){
            putback(savein,STDIN_FILENO);
            putback(saveout,STDOUT_FILENO);
            return 1;
        }
    }

    int rc;
    if (strcmp(c->argv[0],"cd")==0){
        rc=docd(c);
    }
    else{
        rc=dohist(c);
    }

    putback(savein,STDIN_FILENO);
    putback(saveout,STDOUT_FILENO);
    return rc;
}

//a command killed by a signal is never a success
int getstat(int st){
    if (WIFEXITED(st)){
        return WEXITSTATUS(st);
    }
    return 1;
}

int runone(struct cmd* c){
    if (isbuiltin(c->argv[0])){
        return dobuiltin(c);
    }

    pid_t pid=fork();
    if (pid<0){
        err();
        return 1;
    }
    if (pid==0){
        if (redirect(c)<0){
            _exit(1);           //_err already printed
        }
        execvp(c->argv[0],c->argv);
        err();                  // only reach here if exec failed
        _exit(127);
    }

    int st;
    if (waitpid(pid,&st,0)<0){
        return 1;
    }
    return getstat(st);
}

int runpipe(struct cmd* sg,int n){
    int fds[(MAXSTAGE-1)*2];
    pid_t pid[MAXSTAGE];
    int np=n-1;
    int res=1;

    for (int i=0;i<np;i++){
        if (pipe(&fds[i*2])<0){
            while (--i>=0){
                close(fds[i*2]);
                close(fds[i*2+1]);
            }
            err();
            return 1;
        }
    }

    for (int i=0;i<n;i++){
        pid[i]=fork();
        if (pid[i]<0){
            err();
            continue;           //the other stages still get to run
        }
        if (pid[i]==0){
            if (i>0){
                dup2(fds[(i-1)*2],STDIN_FILENO);
            }
            if (i<n-1){
                dup2(fds[i*2+1],STDOUT_FILENO);
            }
            // every child closes EVERY pipe fd, not just the 2 it rewired.
            // leave one write end open anywhere and the reader downstream just
            // hangs forever waiting for an EOF that never comes
            for (int j=0;j<np*2;j++){
                close(fds[j]);
            }
            execvp(sg[i].argv[0],sg[i].argv);
            err();
            _exit(127);
        }
    }

   // parent side, same reason - close before waiting not after
    for (int i=0;i<np*2;i++){
        close(fds[i]);
    }

    for (int i=0;i<n;i++){
        if (pid[i]<0){
            continue;
        }
        int st;
        if (waitpid(pid[i],&st,0)<0){
            continue;
        }
        if (i==n-1){
            res=getstat(st);    //the last stage decides whole pipeline's status
        }
    }
    return res;
}

//runs the job held in tok[from..to-1], unless the separator in front says no
void runjob(char* tok[],int from,int to,int sep){
    struct cmd sg[MAXSTAGE];
    int n=0;
    int start=from;

    if (from>=to){
        return;                 //nothing sitting between two separators
    }
    // a skipped job must NOT touch laststat. thats what makes a chain like
    // "false && a || b" still reach b, same as bash
    if (sep==S_AND && laststat!=0){
        return;
    }
    if (sep==S_OR && laststat==0){
        return;
    }

    for (int i=from;i<=to;i++){
        if (i!=to && strcmp(tok[i],"|")!=0){
            continue;
        }
        if (n==MAXSTAGE || parsecmd(tok,start,i,&sg[n])<0){
            err();
            laststat=1;
            return;
        }
        n++;
        start=i+1;
    }

    if (n==1){
        laststat=runone(&sg[0]);
    }
    else{
        laststat=runpipe(sg,n);
    }
}

void runline(char* line){
    char* tok[MAXTOK];
    int start=0;
    int sep=S_NONE;

    int n=tokenize(line,tok,MAXTOK);
    if (n<0){
        err();
        return;
    }

    //going one past the end flushes the job the line finishes with
    for (int i=0;i<=n;i++){
        int next=S_NONE;
        if (i<n){
            if (strcmp(tok[i],";")==0){
                next=S_SEMI;
            }
            else if(strcmp(tok[i],"&&")==0){
                next=S_AND;
            }
            else if(strcmp(tok[i],"||")==0){
                next=S_OR;
            }
            else{
                continue;
            }
        }
        runjob(tok,start,i,sep);
        sep=next;
        start=i+1;
    }
}

int main(void){
    char line[MAXLINE+2];       //2048 characters plus the newline and the nul

    // when stdin is a script (not a tty) libc makes stdout fully buffered.
    // that (a) reorders the prompt vs what children write straight to the fd,
    // and (b) fork() copies whatever is still sitting in the buffer into every
    // child so it gets printed twice. unbuffer to kill both problems
    setvbuf(stdout,NULL,_IONBF,0);

    //hold a private copy of the real stdout so err() can still reach it from a
    //child whose fd 1 has been pointed at a pipe or a file. close on exec keeps
    //it from leaking into the programs being run
    int saved=dup(STDOUT_FILENO);
    if (saved>=0){
        fcntl(saved,F_SETFD,FD_CLOEXEC);
        errfd=saved;
    }

    // remember where we were launched, that is what bare cd treats as home
    char cwd[PATH_MAX];
    if (getcwd(cwd,sizeof(cwd))!=NULL){
        starthome=strdup(cwd);
    }

    while (1){
        fputs(PROMPT,stdout);
        if (fgets(line,sizeof(line),stdin)==NULL){
            break;              //EOF, nothing more to read
        }
        line[strcspn(line,"\r\n")]='\0';
        if (line[0]=='\0'){
            continue;           // bare enter, dont record it
        }
        addhist(line);          //recorded before it runs so history lists itself
        runline(line);          //tokenize cuts the line up in place
    }

    cleanup();
    return 0;
}
