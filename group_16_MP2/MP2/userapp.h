#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef USERAPP_H_
#define USERAPP_H_

#include<stdlib.h>
#include<sys/time.h>
#include<stdio.h>

void REGISTER(int PID,int Period,int JobProcessTime);
void YIELD(int PID);
void UNREGISTER(int PID);
int process_in_the_list(int pid);
long do_job(int n);
#endif