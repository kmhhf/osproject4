#include <stdio.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/msg.h>
#include <sys/types.h>

// struct for process control block
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

//struct for simulated clock
struct Clock
{
    long seconds;
    long nanoSeconds;
};

//struct for shared message
struct message
{
    long mtype;
};

int main(int argc, char *argv[0])
{
    struct message messageReceive;
    struct message messageSend;

    //set mtype to send back to oss
    messageSend.mtype = 19;

    //get key and shared message
    key_t msgKey = ftok("oss", 1);
    int msgId = msgget(msgKey, 0666);
    if (msgId == -1)
    {
        fprintf(stderr, "%s: Error: ", argv[0]);
        perror("");
        exit(EXIT_FAILURE);
    }

    //get key and shared mem for PCB
    key_t pcbKey = ftok("oss",2);
    int pcbShmid = shmget(pcbKey, sizeof(struct PCB) * 18, 0666);
    if(pcbShmid == -1)
    {
        fprintf(stderr, "%s: Error: ", argv[0]);
        perror("");
        exit(EXIT_FAILURE);
    }

    struct PCB* sharedPcb = shmat(pcbShmid, NULL, 0);
    if(sharedPcb == -1)
    {
        fprintf(stderr, "%s: Error: ", argv[0]);
        perror("");
        exit(EXIT_FAILURE);
    }

    //get key and shared mem for shared clock
    key_t clockKey = ftok("oss", 3);
    int clockShmid = shmget(clockKey, sizeof(struct Clock), 0666);
    if(clockShmid == -1)
    {
        fprintf(stderr, "%s: Error: ", argv[0]);
        perror("");
        exit(EXIT_FAILURE);
    }

    struct Clock* sharedClock = shmat(clockShmid, NULL, 0);
    if(sharedClock == -1)
    {
        fprintf(stderr, "%s: Error: ", argv[0]);
        perror("");
        exit(EXIT_FAILURE);
    }

    int pcbIndex = atoi(argv[1]);
    int random;
    int randomNano;
    int quantum = 10000000;

    //set seed for rand
    srand(getpid());

    while(1)
    {
        quantum = 10000000;

        //wait for message from OSS to get cpu
        msgrcv(msgId, &messageReceive, sizeof(struct message), pcbIndex + 1, 0);

        //time calculations
        sharedPcb[pcbIndex].waitSeconds = (sharedClock->seconds - sharedPcb[pcbIndex].timeLastBurstSeconds);
        if(sharedClock->nanoSeconds < sharedPcb[pcbIndex].timeLastBurstNanoSeconds)
        {
            sharedPcb[pcbIndex].waitSeconds = sharedPcb[pcbIndex].waitSeconds - 1;
        }

        sharedPcb[pcbIndex].waitNanoSeconds = (sharedClock->nanoSeconds - sharedPcb[pcbIndex].timeLastBurstNanoSeconds);
        if(sharedPcb[pcbIndex].waitNanoSeconds < 0)
        {
            sharedPcb[pcbIndex].waitNanoSeconds = 1000000000 + sharedPcb[pcbIndex].waitNanoSeconds;
        }

        if(sharedPcb[pcbIndex].waitNanoSeconds > 1000000000)
        {
            sharedPcb[pcbIndex].waitNanoSeconds = sharedPcb[pcbIndex].waitNanoSeconds - 1000000000;
            sharedPcb[pcbIndex].waitSeconds = sharedPcb[pcbIndex].waitSeconds + 1;
        }

        //set quantum depending on which queue the process was in
        int priority = sharedPcb[pcbIndex].priority;
        if(priority == 0)
        {
            quantum = quantum * 2;
        }
        else if (priority == 2)
        {
            quantum = quantum / 2;
        }
        else if (priority == 3)
        {
            quantum = quantum / 4;
        }

        //Random determine if it needs the full quantum
        random = rand() % 2;
        if(random == 1)
        {
            quantum = rand() % quantum;
        }

        //Random 1 in 4 chance to terminate
        random = rand() % 4;
        if (random == 0)
        {
            sharedPcb[pcbIndex].lastBurstNanoSeconds = rand() % quantum;
            sharedPcb[pcbIndex].terminate = 1;

            //send message back to OSS
            msgsnd(msgId, &messageSend, sizeof(struct message), 0);
            shmdt(sharedClock);
            shmdt(sharedPcb);
            exit(EXIT_FAILURE);
        }

        // Random to determine if the process will use up the entire time slice.
        random = rand() % 2;
        if(random == 0)
        {
            sharedPcb[pcbIndex].lastBurstNanoSeconds = quantum;

            //send message back to OSS
            msgsnd(msgId, &messageSend, sizeof(struct message), 0);
        }

            // determine if the process is blocked or preempted and for how long
        else
        {
            random = rand() % 4;
            randomNano = rand() % 1001;

            //determine if the process was preempted
            if(random == 3)
            {
                random = (rand() % 99) + 1;
                sharedPcb[pcbIndex].lastBurstNanoSeconds = quantum / 100 * random;
                sharedPcb[pcbIndex].preempted = 1;

                //send message back to OSS
                msgsnd(msgId, &messageSend, sizeof(struct message), 0);
            }

                //message is blocked. report in PCB
            else
            {
                sharedPcb[pcbIndex].blocked = 1;
                sharedPcb[pcbIndex].lastBurstNanoSeconds = rand() % quantum;
                sharedPcb[pcbIndex].blockedTimeSeconds = random;
                sharedPcb[pcbIndex].blockedTimeNanoSeconds = randomNano;

                //send message back to OSS
                msgsnd(msgId, &messageSend, sizeof(struct message), 0);
            }
        }
    }
    return 0;
}
