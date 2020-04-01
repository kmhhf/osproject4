//
// Created by Kyle on 4/1/2020.
//

#include <stdio.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/msg.h>
#include <sys/types.h>
#include <string.h>
#include <signal.h>

int msgId;
int clockShmid;
int pcbShmid;
int timeout = 0;

//Process control block to keep track of processes and their information
struct PCB
{
    int simPid;
    int processPid;
    int priority;
    long startTimeSeconds;
    long startTimeNanoSeconds;
    long totalCpuSeconds;
    long totalCpuNanoSeconds;
    long totalTimeSeconds;
    long totalTimeNanoSeconds;
    long lastBurstNanoSeconds;
    long timeLastBurstSeconds;
    long timeLastBurstNanoSeconds;
    long waitSeconds;
    long waitNanoSeconds;
    int terminate;
    int blocked;
    long blockedTimeSeconds;
    long blockedTimeNanoSeconds;
    int preempted;
};

//simulated clock
struct Clock
{
    long seconds;
    long nanoSeconds;
};

//struct for queue nodes
struct node
{
    int pid;
    struct node* next;
};

//struct for queue
struct queue
{
    struct node* first;
    struct node* last;
};

//message for shared message
struct message
{
    long mtype;
};

//signal handler to stop forks after timer
void timer_handler(int signum)
{
    fprintf(stderr, "Program timedout.  Stopping creating children.\n");
    timeout = 1;
}

// ctrl C handler
void ctrlc_handler(int signum)
{
    fprintf(stderr, "\n^C interrupt received.\n");
    shmctl(pcbShmid, IPC_RMID, NULL);
    msgctl(msgId, IPC_RMID, NULL);
    shmctl(clockShmid, IPC_RMID, NULL);
    kill(0, SIGKILL);
}

void createQueue(struct queue* que);
void enqueue(struct queue* que, int pid);
int dequeue(struct queue* que);

int main(int argc, char *argv[0])
{
    int opt;
    int random;
    int bitmap[1];
    bitmap[0] = 0;
    int simPid;
    long totalTimeAllSeconds = 0;
    long totalTimeAllNanoSeconds = 0;
    int totalBlocked = 0;
    long totalBlockedTime = 0;
    long totalWaitTime = 0;
    long totalCpuIdleTime = 0;
    long totalCpuTime = 0;
    long timeCalc1;
    long timeCalc2;
    struct queue* high;
    struct queue* mid;
    struct queue* low;
    struct queue* blocked;
    struct queue* realTime;
    FILE* log = NULL;
    struct message messageSend;
    struct message messageReceive;

    //signals for signal handlers
    signal(SIGALRM, timer_handler);
    signal(SIGINT, ctrlc_handler);

    //handle options
    while ((opt = getopt(argc, argv, "h")) != -1)
    {
        switch (opt)
        {
            case 'h':
                printf("Usage: %s [-h]\n", argv[0]);
                exit(EXIT_FAILURE);
            default:
                fprintf(stderr, "Usage: %s [-h]\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    //send signal after 3 seconds
    alarm(3);

    //open log.txt for writting
    log = fopen("log.txt", "w");
    if(log == NULL)
    {
        fprintf(stderr, "%s: Error: ", argv[0]);
        perror("");
        exit(EXIT_FAILURE);
    }

    //get key and create shared message
    key_t msgKey = ftok("oss", 1);
    msgId = msgget(msgKey, IPC_CREAT | 0666);
    if (msgId == -1)
    {
        fprintf(stderr, "%s: Error: ", argv[0]);
        perror("");
        exit(EXIT_FAILURE);
    }

    //get key and create shared memory for PCB
    key_t pcbKey = ftok("oss", 2);
    pcbShmid = shmget(pcbKey, sizeof(struct PCB) * 18, IPC_CREAT | 0666);
    if(pcbShmid == -1)
    {
        fprintf(stderr, "%s: Error: ", argv[0]);
        perror("");
        msgctl(msgId, IPC_RMID, NULL);
        exit(EXIT_FAILURE);
    }

    //attach to PCB shared memory
    struct PCB* sharedPcb = shmat(pcbShmid, NULL, 0);
    if(sharedPcb == -1)
    {
        fprintf(stderr, "%s: Error: ", argv[0]);
        perror("");
        shmctl(pcbShmid, IPC_RMID, NULL);
        msgctl(msgId, IPC_RMID, NULL);
        exit(EXIT_FAILURE);
    }

    //get key and create shared memory for simulated clock
    key_t clockKey = ftok("oss", 3);
    clockShmid = shmget(clockKey, sizeof(struct Clock), IPC_CREAT | 0666);
    if(clockShmid == -1)
    {
        fprintf(stderr, "%s: Error: ", argv[0]);
        perror("");
        shmctl(pcbShmid, IPC_RMID, NULL);
        msgctl(msgId, IPC_RMID, NULL);
        exit(EXIT_FAILURE);
    }

    //attach to shared memory for clock
    struct Clock* sharedClock = shmat(clockShmid, NULL, 0);
    if(sharedClock == -1)
    {
        fprintf(stderr, "%s: Error: ", argv[0]);
        perror("");
        shmctl(pcbShmid, IPC_RMID, NULL);
        msgctl(msgId, IPC_RMID, NULL);
        shmctl(clockShmid, IPC_RMID, NULL);
        exit(EXIT_FAILURE);
    }

    sharedClock->seconds = 0;
    sharedClock->nanoSeconds = 0;

    high = malloc(sizeof(struct queue));
    mid = malloc(sizeof(struct queue));
    low = malloc(sizeof(struct queue));
    blocked = malloc(sizeof(struct queue));
    realTime = malloc(sizeof(struct queue));
    createQueue(high);
    createQueue(mid);
    createQueue(low);
    createQueue(blocked);
    createQueue(realTime);
    int totalProcesses = 0;
    int activeChildren = 0;

    srand(getpid());

    while((totalProcesses < 100 && timeout == 0)|| activeChildren > 0)
    {
        if(totalProcesses < 100 && timeout == 0)
        {
            int i = 0;
            for(i = 0; i < 18; i++)
            {
                if(bitmap[0] & (1 << i))
                {
                    continue;
                }
                else
                {
                    bitmap[0] |= (1 << i);
                    simPid = i;
                    sharedPcb[i].simPid = i;
                    sharedPcb[i].processPid = totalProcesses;
                    sharedPcb[i].priority = 1;
                    sharedPcb[i].startTimeSeconds = sharedClock->seconds;
                    sharedPcb[i].startTimeNanoSeconds = sharedClock->nanoSeconds;
                    sharedPcb[i].totalCpuSeconds = 0;
                    sharedPcb[i].totalCpuNanoSeconds = 0;
                    sharedPcb[i].totalTimeSeconds = 0;
                    sharedPcb[i].totalTimeNanoSeconds = 0;
                    sharedPcb[i].lastBurstNanoSeconds = 0;
                    sharedPcb[i].timeLastBurstSeconds = 0;
                    sharedPcb[i].timeLastBurstNanoSeconds = 0;
                    sharedPcb[i].waitSeconds = 0;
                    sharedPcb[i].waitNanoSeconds = 0;
                    sharedPcb[i].terminate = 0;
                    sharedPcb[i].blocked = 0;
                    sharedPcb[i].blockedTimeSeconds = 0;
                    sharedPcb[i].blockedTimeNanoSeconds = 0;
                    sharedPcb[i].preempted = 0;

                    pid_t processPid = fork();
                    if(processPid == -1)
                    {
                        fprintf(stderr, "%s: Error: ", argv[0]);
                        perror("");
                        shmdt(sharedClock);
                        shmdt(sharedPcb);
                        shmctl(pcbShmid, IPC_RMID, NULL);
                        shmctl(clockShmid, IPC_RMID, NULL);
                        msgctl(msgId, IPC_RMID, NULL);
                    }

                    if(processPid == 0)
                    {
                        char processIndex[12];
                        sprintf(processIndex, "%d", simPid);
                        execl("./process", "process", processIndex, NULL);
                        fprintf(stderr, "%s: Error: execl failed.", argv[0]);
                    }
                    totalProcesses++;
                    activeChildren++;
                    sharedPcb[simPid].startTimeSeconds = sharedClock->seconds;
                    sharedPcb[simPid].startTimeNanoSeconds = sharedClock->nanoSeconds;

                    if(rand() % 10 == 0)
                    {
                        enqueue(realTime, simPid);
                        fprintf(log, "OSS: Gernerating process with PID %d and putting it in real-time que at %d:%d\n", sharedPcb[simPid].processPid, sharedClock->seconds, sharedClock->nanoSeconds);
                    }
                    else
                    {
                        enqueue(high, simPid);
                        fprintf(log, "OSS: Gernerating process with PID %d and putting it in que 1 at %d:%d\n", sharedPcb[simPid].processPid, sharedClock->seconds, sharedClock->nanoSeconds);
                    }
                    sharedClock->nanoSeconds = sharedClock->nanoSeconds + rand() % 5000;
                    break;
                }
            }
        }
        while(isEmpty(realTime) == 0)
        {
            simPid = dequeue(realTime);
            fprintf(log, "OSS: Dispatching process with PID %d from real-time queue at time %d:%d\n", sharedPcb[simPid].processPid, sharedClock->seconds, sharedClock->nanoSeconds);
            messageSend.mtype = simPid + 1;
            msgsnd(msgId, &messageSend, sizeof(struct message), 0);
            msgrcv(msgId, &messageReceive, sizeof(struct message), 19, 0);
            fprintf(log, "OSS: Receiving that process with PID %d ran for %d nanoseconds\n", sharedPcb[simPid].processPid, sharedPcb[simPid].lastBurstNanoSeconds);
            sharedPcb[simPid].totalCpuNanoSeconds = sharedPcb[simPid].totalCpuNanoSeconds + sharedPcb[simPid].lastBurstNanoSeconds;
            sharedPcb[simPid].timeLastBurstSeconds = sharedClock->seconds;
            sharedPcb[simPid].timeLastBurstNanoSeconds = sharedClock->nanoSeconds;

            if(sharedPcb[simPid].terminate == 1)
            {
                fprintf(log, "OSS: Process with PID %d terminated at %d:%d\n", sharedPcb[simPid].processPid, sharedClock->seconds, sharedClock->nanoSeconds);
                bitmap[0] &= ~(1 << simPid);
                activeChildren--;
                waitpid(-1, NULL, WNOHANG);
                timeCalc1 = (sharedClock->seconds * 1000000000) + sharedClock->nanoSeconds;
                timeCalc2 = (sharedPcb[simPid].startTimeSeconds * 1000000000) + sharedPcb[simPid].startTimeNanoSeconds;
                sharedPcb[simPid].totalTimeSeconds = (timeCalc1 - timeCalc2) / 1000000000;
                sharedPcb[simPid].totalTimeNanoSeconds = (timeCalc1 - timeCalc2) % 1000000000;
                totalTimeAllSeconds = totalTimeAllSeconds + sharedPcb[simPid].totalTimeSeconds;
                totalTimeAllNanoSeconds = totalTimeAllNanoSeconds + sharedPcb[simPid].totalTimeNanoSeconds;
                totalCpuTime = totalCpuTime + sharedPcb[simPid].totalCpuNanoSeconds;
                if(totalTimeAllNanoSeconds > 1000000000);
                {
                    totalTimeAllSeconds = totalTimeAllSeconds + 1;
                    totalTimeAllNanoSeconds = totalTimeAllNanoSeconds - 1000000000;
                }
            }
            else
            {
                enqueue(realTime, simPid);
            }

        }
        if(isEmpty(high) == 0)
        {
            simPid = dequeue(high);
            fprintf(log, "OSS: Dispatching process with PID %d from queue 1 at time %d:%d\n", sharedPcb[simPid].processPid, sharedClock->seconds, sharedClock->nanoSeconds);
            messageSend.mtype = simPid + 1;
            msgsnd(msgId, &messageSend, sizeof(struct message), 0);
            msgrcv(msgId, &messageReceive, sizeof(struct message), 19, 0);
            fprintf(log, "OSS: Receiving that process with PID %d ran for %d nanoseconds\n", sharedPcb[simPid].processPid, sharedPcb[simPid].lastBurstNanoSeconds);
            sharedPcb[simPid].totalCpuNanoSeconds = sharedPcb[simPid].totalCpuNanoSeconds + sharedPcb[simPid].lastBurstNanoSeconds;
            sharedPcb[simPid].timeLastBurstSeconds = sharedClock->seconds;
            sharedPcb[simPid].timeLastBurstNanoSeconds = sharedClock->nanoSeconds;
            if(sharedPcb[simPid].blocked == 1)
            {
                fprintf(log, "OSS: Process with PID %d was blocked and put in blocked queue at %d:%d\n", sharedPcb[simPid].processPid, sharedClock->seconds, sharedClock->nanoSeconds);
                enqueue(blocked, simPid);
                sharedPcb[simPid].blocked = 0;
                totalBlocked++;
                totalBlockedTime = totalBlockedTime + ((sharedPcb[simPid].blockedTimeSeconds * 1000000000) + sharedPcb[simPid].blockedTimeNanoSeconds);
            }
            else if(sharedPcb[simPid].preempted == 1)
            {
                enqueue(high, simPid);
                fprintf(log, "OSS: Process with PID %d was preempted by another process at %d:%d\n", sharedPcb[simPid].processPid, sharedClock->seconds, sharedClock->nanoSeconds);
                sharedPcb[simPid].preempted = 0;
            }
            else if(sharedPcb[simPid].terminate == 1)
            {
                fprintf(log, "OSS: Process with PID %d terminated at %d:%d\n", sharedPcb[simPid].processPid, sharedClock->seconds, sharedClock->nanoSeconds);
                bitmap[0] &= ~(1 << simPid);
                waitpid(-1, NULL, WNOHANG);
                waitpid(-1, NULL, WNOHANG);
                activeChildren--;
                timeCalc1 = (sharedClock->seconds * 1000000000) + sharedClock->nanoSeconds;
                timeCalc2 = (sharedPcb[simPid].startTimeSeconds * 1000000000) + sharedPcb[simPid].startTimeNanoSeconds;
                sharedPcb[simPid].totalTimeSeconds = (timeCalc1 - timeCalc2) / 1000000000;
                sharedPcb[simPid].totalTimeNanoSeconds = (timeCalc1 -timeCalc2) % 1000000000;
                totalTimeAllSeconds = totalTimeAllSeconds + sharedPcb[simPid].totalTimeSeconds;
                totalTimeAllNanoSeconds = totalTimeAllNanoSeconds + sharedPcb[simPid].totalTimeNanoSeconds;
                if(totalTimeAllNanoSeconds > 1000000000);
                {
                    totalTimeAllSeconds = totalTimeAllSeconds + 1;
                    totalTimeAllNanoSeconds = totalTimeAllNanoSeconds - 1000000000;
                }
                totalWaitTime = totalWaitTime + ((sharedPcb[simPid].waitSeconds * 1000000000) + sharedPcb[simPid].waitNanoSeconds);
                totalCpuTime = totalCpuTime + sharedPcb[simPid].totalCpuNanoSeconds;
            }
            else
            {
                enqueue(mid, simPid);
                sharedPcb[simPid].priority = 2;
            }
        }


        else if(isEmpty(mid) == 0)
        {
            simPid = dequeue(mid);
            fprintf(log, "OSS: Dispatching process with PID %d from queue 2 at time %d:%d\n", sharedPcb[simPid].processPid, sharedClock->seconds, sharedClock->nanoSeconds);
            messageSend.mtype = simPid + 1;
            msgsnd(msgId, &messageSend, sizeof(struct message), 0);
            msgrcv(msgId, &messageReceive, sizeof(struct message), 19, 0);
            fprintf(log, "OSS: Receiving that process with PID %d ran for %d nanoseconds\n", sharedPcb[simPid].processPid, sharedPcb[simPid].lastBurstNanoSeconds);
            sharedPcb[simPid].totalCpuNanoSeconds = sharedPcb[simPid].totalCpuNanoSeconds + sharedPcb[simPid].lastBurstNanoSeconds;
            sharedPcb[simPid].timeLastBurstSeconds = sharedClock->seconds;
            sharedPcb[simPid].timeLastBurstNanoSeconds = sharedClock->nanoSeconds;
            if(sharedPcb[simPid].blocked == 1)
            {
                fprintf(log, "OSS: Process with PID %d was blocked and put in blocked queue at %d:%d\n", sharedPcb[simPid].processPid, sharedClock->seconds, sharedClock->nanoSeconds);
                enqueue(blocked, simPid);
                sharedPcb[simPid].blocked = 0;
                totalBlocked++;
                totalBlockedTime = totalBlockedTime + ((sharedPcb[simPid].blockedTimeSeconds * 1000000000) + sharedPcb[simPid].blockedTimeNanoSeconds);
            }
            else if(sharedPcb[simPid].preempted == 1)
            {
                fprintf(log, "OSS: Process with PID %d was preempted by another process at %d:%d\n", sharedPcb[simPid].processPid, sharedClock->seconds, sharedClock->nanoSeconds);
                enqueue(high, simPid);
                sharedPcb[simPid].preempted = 0;
                sharedPcb[simPid].priority = 1;
            }
            else if(sharedPcb[simPid].terminate == 1)
            {
                fprintf(log, "OSS: Process with PID %d terminated at %d:%d\n", sharedPcb[simPid].processPid, sharedClock->seconds, sharedClock->nanoSeconds);
                bitmap[0] &= ~(1 << simPid);
                waitpid(-1, NULL, WNOHANG);
                waitpid(-1, NULL, WNOHANG);
                activeChildren--;
                timeCalc1 = (sharedClock->seconds * 1000000000) + sharedClock->nanoSeconds;
                timeCalc2 =(sharedPcb[simPid].startTimeSeconds * 1000000000) + sharedPcb[simPid].startTimeNanoSeconds;
                sharedPcb[simPid].totalTimeSeconds = (timeCalc1 - timeCalc2) / 1000000000;
                sharedPcb[simPid].totalTimeNanoSeconds = (timeCalc1 -timeCalc2) % 1000000000;
                totalTimeAllSeconds = totalTimeAllSeconds + sharedPcb[simPid].totalTimeSeconds;
                totalTimeAllNanoSeconds = totalTimeAllNanoSeconds + sharedPcb[simPid].totalTimeNanoSeconds;
                if(totalTimeAllNanoSeconds > 1000000000);
                {
                    totalTimeAllSeconds = totalTimeAllSeconds + 1;
                    totalTimeAllNanoSeconds = totalTimeAllNanoSeconds - 1000000000;
                }
                totalWaitTime = totalWaitTime + ((sharedPcb[simPid].waitSeconds * 1000000000) + sharedPcb[simPid].waitNanoSeconds);
                totalCpuTime = totalCpuTime + sharedPcb[simPid].totalCpuNanoSeconds;
            }
            else
            {
                enqueue(low, simPid);
                sharedPcb[simPid].priority = 3;
            }
        }


        else if(isEmpty(low) == 0)
        {
            simPid = dequeue(low);
            fprintf(log, "OSS: Dispatching process with PID %d from queue 3 at time %d:%d\n", sharedPcb[simPid].processPid, sharedClock->seconds, sharedClock->nanoSeconds);
            messageSend.mtype = simPid + 1;
            msgsnd(msgId, &messageSend, sizeof(struct message), 0);
            msgrcv(msgId, &messageReceive, sizeof(struct message), 19, 0);
            fprintf(log, "OSS: Receiving that process with PID %d ran for %d nanoseconds\n", sharedPcb[simPid].processPid, sharedPcb[simPid].lastBurstNanoSeconds);
            sharedPcb[simPid].totalCpuNanoSeconds = sharedPcb[simPid].totalCpuNanoSeconds + sharedPcb[simPid].lastBurstNanoSeconds;
            sharedPcb[simPid].timeLastBurstSeconds = sharedClock->seconds;
            sharedPcb[simPid].timeLastBurstNanoSeconds = sharedClock->nanoSeconds;
            if(sharedPcb[simPid].blocked == 1)
            {
                fprintf(log, "OSS: Process with PID %d was blocked and put in blocked queue at %d:%d\n", sharedPcb[simPid].processPid, sharedClock->seconds, sharedClock->nanoSeconds);
                enqueue(blocked, simPid);
                sharedPcb[simPid].blocked = 0;
                totalBlocked++;
                totalBlockedTime = totalBlockedTime + ((sharedPcb[simPid].blockedTimeSeconds * 1000000000) + sharedPcb[simPid].blockedTimeNanoSeconds);
            }
            else if(sharedPcb[simPid].preempted == 1)
            {
                fprintf(log, "OSS: Process with PID %d was preempted by another process at %d:%d\n", sharedPcb[simPid].processPid, sharedClock->seconds, sharedClock->nanoSeconds);
                enqueue(high, simPid);
                sharedPcb[simPid].preempted = 0;
                sharedPcb[simPid].priority = 1;
            }
            else if(sharedPcb[simPid].terminate == 1)
            {
                fprintf(log, "OSS: Process with PID %d terminated at %d:%d\n", sharedPcb[simPid].processPid, sharedClock->seconds, sharedClock->nanoSeconds);
                bitmap[0] &= ~(1 << simPid);
                waitpid(-1, NULL, WNOHANG);
                waitpid(-1, NULL, WNOHANG);
                activeChildren--;
                timeCalc1 = (sharedClock->seconds * 1000000000) + sharedClock->nanoSeconds;
                timeCalc2 = (sharedPcb[simPid].startTimeSeconds * 1000000000) + sharedPcb[simPid].startTimeNanoSeconds;
                sharedPcb[simPid].totalTimeSeconds = (timeCalc1 - timeCalc2) / 1000000000;
                sharedPcb[simPid].totalTimeNanoSeconds = (timeCalc1 -timeCalc2) % 1000000000;
                totalTimeAllSeconds = totalTimeAllSeconds + sharedPcb[simPid].totalTimeSeconds;
                totalTimeAllNanoSeconds = totalTimeAllNanoSeconds + sharedPcb[simPid].totalTimeNanoSeconds;
                if(totalTimeAllNanoSeconds > 1000000000);
                {
                    totalTimeAllSeconds = totalTimeAllSeconds + 1;
                    totalTimeAllNanoSeconds = totalTimeAllNanoSeconds - 1000000000;
                }
                totalWaitTime = totalWaitTime + ((sharedPcb[simPid].waitSeconds * 1000000000) + sharedPcb[simPid].waitNanoSeconds);
                totalCpuTime = totalCpuTime + sharedPcb[simPid].totalCpuNanoSeconds;
            }
            else
            {
                enqueue(low, simPid);
            }
        }

        else
        {
            random = rand() % 5000;
            totalCpuIdleTime = totalCpuIdleTime + random;
        }
        random = rand() % 901 + 100;
        sharedClock->nanoSeconds = sharedClock->nanoSeconds + random;
        fprintf(log, "OSS: Dispatch took %d nanoseconds\n", random);

        if(isEmpty(blocked) == 0)
        {
            simPid = dequeue(blocked);
            timeCalc1 = ((sharedClock->seconds * 1000000000) + sharedClock->nanoSeconds) - ((sharedPcb[simPid].timeLastBurstSeconds * 1000000000) + sharedPcb[simPid].timeLastBurstNanoSeconds);
            timeCalc2 = (sharedPcb[simPid].blockedTimeSeconds * 1000000000) + sharedPcb[simPid].blockedTimeNanoSeconds;
            if(timeCalc1 > timeCalc2)
            {
                fprintf(log, "OSS: Process with PID %d was moved from blocked queue to queue 1 at %d:%d\n", sharedPcb[simPid].processPid, sharedClock->seconds, sharedClock->nanoSeconds);
                enqueue(high, simPid);
                sharedPcb[simPid].priority = 1;
            }
            else
            {
                enqueue(blocked, simPid);
            }

        }

        if(isEmpty(mid) == 0)
        {
            simPid = dequeue(mid);
            timeCalc1 = ((sharedClock->seconds * 1000000000) + sharedClock->nanoSeconds) - ((sharedPcb[simPid].timeLastBurstSeconds * 1000000000) + sharedPcb[simPid].timeLastBurstNanoSeconds);
            if(timeCalc1 > 2000000000)
            {
                enqueue(high,simPid);
                sharedPcb[simPid].priority = 1;
            }
            else
            {
                enqueue(mid, simPid);
            }
        }


        if(isEmpty(low) == 0)
        {
            simPid = dequeue(low);
            timeCalc1 = (long)((long)((long)sharedClock->seconds * 1000000000) + sharedClock->nanoSeconds) - (long)((long)((long)sharedPcb[simPid].timeLastBurstSeconds * 1000000000) + sharedPcb[simPid].timeLastBurstNanoSeconds);
            if(timeCalc1 > 2000000000)
            {
                enqueue(high,simPid);
                sharedPcb[simPid].priority = 1;
            }
            else
            {
                enqueue(low, simPid);
            }
        }
        sharedClock->seconds = sharedClock->seconds + 1;
        sharedClock->nanoSeconds = sharedClock->nanoSeconds + (rand() % 1001);

    }
    printf("Average total wait time per process was %d seconds and %d nanoseconds\n", (totalWaitTime / 1000000000) / 100, (totalWaitTime % 1000000000) / 100);
    printf("Average CPU usage for each process was %d seconds and %d nanoseconds\n", (totalCpuTime / 1000000000) / 100, (totalCpuTime % 1000000000) / 100);
    printf("Average time in blocked que per process was %d seconds and %d nanoseconds\n", (totalBlockedTime / 1000000000) / 100, (totalBlockedTime % 1000000000) / 100);
    printf("Total CPU idle time was %d nanoseconds\n", totalCpuIdleTime);

    fflush(log);
    fclose(log);
    log = NULL;

    free(high);
    free(mid);
    free(low);
    free(blocked);
    free(realTime);

    shmdt(sharedClock);
    shmdt(sharedPcb);
    shmctl(pcbShmid, IPC_RMID, NULL);
    shmctl(clockShmid, IPC_RMID, NULL);
    msgctl(msgId, IPC_RMID, NULL);

    return 0;
}

void createQueue(struct queue* que)
{
    que->first = NULL;
    que->last = NULL;
}

void enqueue(struct queue* que, int pid)
{
    struct node* temp;
    temp = malloc(sizeof(struct node));
    if(temp == NULL)
    {
        perror("");
        shmctl(pcbShmid, IPC_RMID, NULL);
        msgctl(msgId, IPC_RMID, NULL);
        shmctl(clockShmid, IPC_RMID, NULL);
        exit(EXIT_FAILURE);
    }
    temp->pid = pid;
    temp->next = NULL;

    if(que->first == NULL)
    {
        que->first = temp;
        que->last = temp;
    }

    else
    {
        que->last->next = temp;
        que->last = temp;
    }
}

int dequeue(struct queue* que)
{
    struct node* temp;
    int pid = que->first->pid;
    temp = que->first;
    que->first = que->first->next;
    free(temp);
    return(pid);
}

int isEmpty(struct queue* que)
{
    if(que->first == NULL)
    {
        return 1;
    }
    else
    {
        return 0;
    }
}
