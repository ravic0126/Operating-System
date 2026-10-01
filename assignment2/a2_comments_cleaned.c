// Each job is a child running ./workload; fd 3 is the pipe write end.
// Keep at most one child running at a time.
// Track CPU usage and turn time here; use the child's messages for I/O
// and completion events.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MAXQ 8
#define MAXJ 16
#define NLEN 24
#define INF  1e18

// workload.c accounts for CPU in 1 ms increments, so its clock can lag ours.
// Treat modelled times within this tolerance as the same instant.
#define TOL 15.0

// Extra time allowed for a delayed message; normally this is never reached.
#define GRACE 3000.0

enum{ST_NEW,ST_READY,ST_RUN,ST_BLOCK,ST_DONE};

typedef struct{
    char name[NLEN];
    double arr,cpu,ioev,iodur;   // Values read from the input line.
    pid_t pid;
    int state;
    int lev;                     // Current queue.
    double turn;                 // Time left in the current turn.
    int al;                      // Quanta remaining at this level.
    double rem;                  // CPU time still required.
    double toio;                 // CPU time until the next I/O.
    double slice;                // Start time of the current turn.
    int stopped;                 // Whether waitpid has confirmed that the child stopped.
    int reaped;
}Job;

static int n,S,G,m;              // Number of queues, boost period, gaming mode, and number of jobs.
static double q[MAXQ];           // Quantum for each queue level.
static int alq[MAXQ];            // Allotment for each queue level.
static Job jobs[MAXJ];

static int rq[MAXQ][MAXJ];       // Ready queues, with the head at index 0.
static int rlen[MAXQ];

static int pfd[2];
static struct timespec t0;
static Job *cur;                 // Job that owns the CPU in the scheduler's model.
static Job *live;                // Child that is currently allowed to run.
static int done;
static int idled;
static double qend;              //when the running turn ends
static double guard;
static double nb;                //next boost
static long ts;                  //stamp for every line of one pass

static double now(void){
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (double)(t.tv_sec-t0.tv_sec)*1000.0+(double)(t.tv_nsec-t0.tv_nsec)/1e6;
}

static void pushb(Job *j){
    rq[j->lev][rlen[j->lev]++]=(int)(j-jobs);
}

// A job preempted by a higher-priority job returns to the head of its queue.
static void pushf(Job *j){
    int L=j->lev,i;
    for (i=rlen[L];i>0;i--) rq[L][i]=rq[L][i-1];
    rq[L][0]=(int)(j-jobs);
    rlen[L]++;
}

// Remove the job from any ready queue.
static void yank(Job *j){
    int L,i,k;
    for (L=0;L<n;L++){
        for (i=0,k=0;i<rlen[L];i++){
            if (rq[L][i]!=(int)(j-jobs)) rq[L][k++]=rq[L][i];
        }
        rlen[L]=k;
    }
}

// Return the head of the highest-priority non-empty queue.
static Job *best(void){
    int L;
    for (L=n-1;L>=0;L--){
        if (rlen[L]) return &jobs[rq[L][0]];
    }
    return NULL;
}

static void popf(int L){
    int i;
    for (i=1;i<rlen[L];i++) rq[L][i-1]=rq[L][i];
    rlen[L]--;
}

// Consume the wait status produced by the signal.
// If it exited instead of stopping, reap it here.
static void waitstop(Job *j){
    int st;
    while (1){
        pid_t r=waitpid(j->pid,&st,WUNTRACED);
        if (r<0){
            if (errno==EINTR) continue;
            break;
        }
        if (WIFSTOPPED(st)){
            j->stopped=1;
            break;
        }
        if (WIFEXITED(st)||WIFSIGNALED(st)){
            j->reaped=1;
            j->stopped=0;
            break;
        }
    }
}

// SIGSTOP is asynchronous, so confirm that the child stopped before SIGCONT.
static void freeze(Job *j){
    if (j==NULL||j->reaped||j->stopped) return;
    kill(j->pid,SIGSTOP);
    waitstop(j);
}

static void thaw(Job *j){
    if (j->reaped) return;
    kill(j->pid,SIGCONT);
    j->stopped=0;
}

// Account for one stretch of CPU time in all relevant counters.
static void charge(Job *j,double used){
    if (used<0) used=0;
    j->rem-=used;
    j->turn-=used;
    j->toio-=used;
}

// The turn expired; Rule 4 determines whether the job also changes level.
static void usedturn(Job *j){
    j->al--;
    if (j->al>0){
        printf("%ld QEXPIRE %s Q%d\n",ts,j->name,j->lev);
        j->turn=q[j->lev];
    }
    else if(j->lev>0){
        printf("%ld DEMOTE %s Q%d Q%d\n",ts,j->name,j->lev,j->lev-1);
        j->lev--;
        j->turn=q[j->lev];
        j->al=alq[j->lev];
    }
    else{
        // Q0 is never demoted; refill its allotment and continue at Q0.
        printf("%ld QEXPIRE %s Q0\n",ts,j->name);
        j->turn=q[0];
        j->al=alq[0];
    }
}

static Job *find(const char *s){
    int i;
    for (i=0;i<m;i++){
        if (strcmp(jobs[i].name,s)==0) return &jobs[i];
    }
    return NULL;
}

static void on_io(Job *j,double t){
    // Account for the CPU time the job actually consumed before I/O, rather than
    // relying on the scheduler's wall-clock measurement.
    if (cur==j){
        double u=j->toio;
        if (u>j->rem) u=j->rem;
        charge(j,u);
    }
    else{
        yank(j);
    }
    j->slice=t;
    printf("%ld IO_START %s\n",ts,j->name);
    j->toio=(j->ioev>0)?j->ioev:INF;

    if (!G){
        // Under Rule 4b, an early I/O refills both counters.
        j->turn=q[j->lev];
        j->al=alq[j->lev];
    }
    else if(j->turn<=TOL){
        // I/O takes precedence in the event order when it coincides with turn expiry.
        // The expired turn still counts, so update the level before the job returns.
        usedturn(j);
    }

    j->state=ST_BLOCK;
    if (live==j) live=NULL;      // The I/O continues while the job is blocked; do not stop it.
    if (cur==j) cur=NULL;
    if (j->stopped) thaw(j);     //we froze it mid announcement, let it go
}

static void on_rdy(Job *j){
    waitstop(j);                 // workload.c stops itself immediately after writing RDY.
    printf("%ld IO_END %s\n",ts,j->name);
    j->state=ST_READY;
    pushb(j);                    // Return to the tail of its current queue.
}

static void on_done(Job *j,double t){
    if (cur==j){
        charge(j,j->rem);
    }
    else{
        yank(j);
    }
    j->slice=t;
    printf("%ld FINISH %s\n",ts,j->name);
    j->state=ST_DONE;
    done++;
    if (live==j) live=NULL;
    if (cur==j) cur=NULL;
    if (!j->reaped){
        int st;
        while (waitpid(j->pid,&st,0)<0&&errno==EINTR)
            ;
        j->reaped=1;
    }
}

static char buf[4096];
static size_t fill;

// Parse one complete message and dispatch it to the appropriate handler.
static void msg(const char *ln,double t){
    char kind[8],nm[NLEN];
    Job *j;
    if (sscanf(ln,"%7s %23s",kind,nm)!=2) return;
    j=find(nm);
    if (j==NULL||j->state==ST_DONE) return;
    if (strcmp(kind,"DONE")==0) on_done(j,t);
    else if(strcmp(kind,"IO")==0) on_io(j,t);
    else if(strcmp(kind,"RDY")==0) on_rdy(j);
}

// All jobs share fd 3, so a read may contain several lines or a partial line.
// Process only complete lines and keep any partial line for the next read.
static void drain(double t){
    while (1){
        ssize_t got=read(pfd[0],buf+fill,sizeof buf-1-fill);
        char *nl;
        if (got<0){
            if (errno==EINTR) continue;
            break;                   //EAGAIN, nothing left
        }
        if (got==0) break;
        fill+=(size_t)got;
        buf[fill]='\0';
        while ((nl=memchr(buf,'\n',fill))!=NULL){
            size_t u=(size_t)(nl-buf)+1;
            *nl='\0';
            msg(buf,t);
            memmove(buf,buf+u,fill-u);
            fill-=u;
            buf[fill]='\0';
        }
    }
}

// The quantum expired: update the counters and put the job at the queue tail.
static void expire(double t){
    Job *j=cur;
    charge(j,j->turn);               // Consume the remainder of the current turn.
    j->slice=t;
    usedturn(j);
    j->state=ST_READY;
    pushb(j);
    cur=NULL;
}

static void arrive(Job *j,double t){
    printf("%ld ARRIVE %s\n",ts,j->name);
    j->state=ST_READY;
    j->lev=n-1;                      // New jobs always enter the highest-priority queue.
    j->turn=q[n-1];
    j->al=alq[n-1];
    j->slice=t;
    pushb(j);
}

// Rule 5: move all active jobs to the highest-priority queue and refill their
// counters. Ready jobs are ordered by input order; blocked jobs rejoin on I/O.
//when their io ends
static void boost(double t){
    int i;
    printf("%ld BOOST\n",ts);
    if (cur!=NULL){
        charge(cur,t-cur->slice);
        cur->slice=t;
        cur->state=ST_READY;
        cur=NULL;
    }
    for (i=0;i<n;i++) rlen[i]=0;
    for (i=0;i<m;i++){
        Job *j=&jobs[i];
        if (j->state==ST_NEW||j->state==ST_DONE) continue;
        j->lev=n-1;
        j->turn=q[n-1];
        j->al=alq[n-1];
        if (j->state==ST_READY) pushb(j);
    }
}

// Determine the next event that can require attention.
// Wait for I/O or completion if either happens before the quantum expires;
// only use the quantum deadline when it is the earliest relevant event.
static void arm(Job *j){
    double nx=(j->rem<j->toio)?j->rem:j->toio;
    if (nx<0) nx=0;
    qend=(j->turn+TOL<nx)?j->slice+j->turn:INF;
    guard=j->slice+nx+GRACE;
}

// Dispatch the highest-priority ready job, or enter the idle state.
static void pick(double t){
    Job *b=best();
    if (cur!=NULL){
        // Only a strictly higher-priority arrival can preempt the current job.
        if (b==NULL||b->lev<=cur->lev) return;
        charge(cur,t-cur->slice);
        cur->state=ST_READY;
        pushf(cur);
        cur=NULL;
    }
    if (b==NULL){
        freeze(live);
        live=NULL;
        if (!idled){
            printf("%ld IDLE\n",ts);
            idled=1;
        }
        return;
    }
    popf(b->lev);
    b->state=ST_RUN;
    b->slice=t;
    cur=b;
    if (live!=b){
        freeze(live);
        thaw(b);
        live=b;
    }
    printf("%ld RUN %s Q%d\n",ts,b->name,b->lev);
    idled=0;
    arm(b);
}

static void readin(const char *path){
    FILE *f=fopen(path,"r");
    int i;
    if (f==NULL){
        fprintf(stderr,"a2: cannot open %s\n",path);
        exit(1);
    }
    if (fscanf(f,"%d %d %d",&n,&S,&G)!=3) exit(1);
    // The input lists queues from highest priority to Q0, so store them backwards.
    for (i=n-1;i>=0;i--){
        if (fscanf(f,"%lf",&q[i])!=1) exit(1);
    }
    for (i=n-1;i>=0;i--){
        if (fscanf(f,"%d",&alq[i])!=1) exit(1);
    }
    if (fscanf(f,"%d",&m)!=1) exit(1);
    for (i=0;i<m;i++){
        Job *j=&jobs[i];
        if (fscanf(f,"%23s %lf %lf %lf %lf",j->name,&j->arr,&j->cpu,&j->ioev,&j->iodur)!=5) exit(1);
        j->state=ST_NEW;
        j->rem=j->cpu;
        j->toio=(j->ioev>0)?j->ioev:INF;
        j->lev=n-1;
    }
    fclose(f);
}

static void cleanup(void);

// Fork every job and give each child fd 3 as the pipe's write end.
static void launch(void){
    int i;
    if (pipe(pfd)<0){
        perror("a2: pipe");
        exit(1);
    }
    for (i=0;i<m;i++){
        char c1[32],c2[32],c3[32];
        pid_t p;
        snprintf(c1,sizeof c1,"%.0f",jobs[i].cpu);
        snprintf(c2,sizeof c2,"%.0f",jobs[i].ioev);
        snprintf(c3,sizeof c3,"%.0f",jobs[i].iodur);
        p=fork();
        if (p<0){
            perror("a2: fork");
            exit(1);
        }
        if (p==0){
            close(pfd[0]);
            if (pfd[1]!=3){
                if (dup2(pfd[1],3)<0) _exit(127);
                close(pfd[1]);
            }
            else{
                fcntl(3,F_SETFD,0);
            }
            execl("./workload","workload",jobs[i].name,c1,c2,c3,(char *)NULL);
            _exit(127);
        }
        jobs[i].pid=p;
    }
    // Each child stops itself during startup.
    // Wait until every child is parked before starting the scheduler clock, so no CPU
    // time is charged for setup.
    for (i=0;i<m;i++){
        waitstop(&jobs[i]);
        if (jobs[i].reaped){
            fprintf(stderr,"a2: job %s never started\n",jobs[i].name);
            cleanup();
            exit(1);
        }
    }
    fcntl(pfd[0],F_SETFL,O_NONBLOCK);
}

// Kill and reap any remaining children before exiting.
static void cleanup(void){
    int i;
    for (i=0;i<m;i++){
        Job *j=&jobs[i];
        if (j->pid>0&&!j->reaped){
            int st;
            kill(j->pid,SIGKILL);
            kill(j->pid,SIGCONT);
            while (waitpid(j->pid,&st,0)<0&&errno==EINTR)
                ;
            j->reaped=1;
        }
    }
}

int main(int argc,char **argv){
    static char ob[1<<16];
    int i;

    if (argc!=2){
        fprintf(stderr,"usage: %s <input file>\n",argv[0]);
        return 1;
    }
    setvbuf(stdout,ob,_IOFBF,sizeof ob);
    readin(argv[1]);
    launch();

    clock_gettime(CLOCK_MONOTONIC,&t0);
    nb=(S>0)?S:INF;

    // At t = 0, admit jobs that have already arrived and dispatch if possible.
    ts=0;
    for (i=0;i<m;i++){
        if (jobs[i].state==ST_NEW&&jobs[i].arr<=0) arrive(&jobs[i],0.0);
    }
    pick(0.0);

    while (done<m){
        double dl=INF,t,w;
        struct timespec tv;
        struct pollfd pf;

        // Find the earliest relevant deadline: arrival, boost, or turn expiry.
        for (i=0;i<m;i++){
            if (jobs[i].state==ST_NEW&&jobs[i].arr<dl) dl=jobs[i].arr;
        }
        if (nb<dl) dl=nb;
        if (cur!=NULL){
            if (qend<dl) dl=qend;
            if (guard<dl) dl=guard;
        }

        // Wait until the next deadline or until a child sends a message.
        t=now();
        w=dl-t;
        if (w<0) w=0;
        tv.tv_sec=(time_t)(w/1000.0);
        tv.tv_nsec=(long)((w-(double)tv.tv_sec*1000.0)*1e6);
        pf.fd=pfd[0];
        pf.events=POLLIN;
        pf.revents=0;
        ppoll(&pf,1,(dl<INF)?&tv:NULL,NULL);

        t=now();
        // ppoll can wake slightly late. Use the intended deadline when it is already due
        // so that scheduler wake-up overhead does not accumulate across turns.
        //us - else the wakeup cost piles up every turn
        if (dl<INF&&dl<=t&&t-dl<50.0) t=dl;
        ts=(long)(t+0.5);

        // Process events in the order required by Clarification 7: messages, expiry,
        // arrivals, boost, and finally dispatch.
        drain(t);
        if (done>=m) break;          // No events are printed after the final FINISH.

        if (cur!=NULL){
            if (qend<=t){
                expire(t);
            }
            else if(guard<=t){
                guard=t+GRACE;       //message is late, keep waiting
            }
        }

        for (i=0;i<m;i++){
            if (jobs[i].state==ST_NEW&&jobs[i].arr<=t) arrive(&jobs[i],t);
        }

        if (nb<=t){
            boost(t);
            nb+=S;
        }

        pick(t);
    }

    cleanup();
    fflush(stdout);
    return 0;
}
