#include "userapp.h"

#define length 100



void REGISTER(int PID,int Period,int JobProcessTime)
{
    char command[length];
    sprintf(command, "echo \"R, %u, %u, %u\" > /proc/mp2/status", PID, Period, JobProcessTime);
    system(command);
}

void YIELD(int PID)
{
    char command[length];
    sprintf(command, "echo \"Y, %u\" > /proc/mp2/status", PID);
    system(command);
}

void UNREGISTER(int PID)
{
    char command[length];
    sprintf(command, "echo \"D, %u\" > /proc/mp2/status", PID);
    system(command);
}

int process_in_the_list(int pid)
{

    char *buffer = NULL;
    size_t size = 0;

    FILE* file;
    file = fopen("/proc/mp2/status","r");
    int tmp_pid;
    while(-1 != getline(&buffer, &size, file)){
        sscanf(buffer, "Pid:%u",&tmp_pid);
        fprintf(stderr,"%i\n",tmp_pid);
        if(tmp_pid == pid){
            free(buffer);
            fclose(file);
            return 1;
        }
    }
    free(buffer);
    fclose(file);
    return 0;

}

long int do_job(int n)
{
    if (n >= 1)
        return n*do_job(n-1);
    else
        return 1;
}

int main(int argc, char* argv[])
{
    struct timeval t0,start,stop;
    int i;
    int pid = getpid();
    REGISTER(pid, atoi(argv[1]), atoi(argv[2])); //Proc filesystem

    if (!process_in_the_list(pid)){
        fprintf(stdout,"%s\n", "exit_early");
        exit(1);
    }
    gettimeofday(&t0,NULL);
    YIELD(pid); //Proc filesystem
    int existing_jobs = atoi(argv[3]);
    int a;
    for(i = 0; i < existing_jobs; i++)
    {
        gettimeofday(&start,NULL);
        long wakeup_time = (start.tv_sec-t0.tv_sec)*1000000 + start.tv_usec-t0.tv_usec;

        printf("job %u wakeup_time: %ld us\n",pid,wakeup_time);
        for(int j = 0; j < 1049182; j++){
            int b = do_job(102);
            a+= b;
        }

        gettimeofday(&stop,NULL);
        long process_time = (stop.tv_sec-start.tv_sec)*1000000 + stop.tv_usec-start.tv_usec;
        printf("job %u process_time: %ld us\n",pid,process_time);
        YIELD(pid); //ProcFS. JobProcessTime=gettimeofday()-wakeup_time
    } 
    UNREGISTER(pid); //ProcFS
}