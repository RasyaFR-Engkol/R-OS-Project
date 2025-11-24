#include <rossys.hpp>
#include <rosval.h>
#include <userland/syscall.hpp>
#include "task.hpp"

namespace Tasking {
	Task *TaskArray[MAX_TASK];
	U64 ActiveTask = 0;
	U64 CurrentTaskIndex = MAX_TASK;
	BOOL SchedulerActive = FALSE;
	U64 g_ForegroundPID = -1;

	Task *GetTaskPID(U64 pid){
		Task *Foundtask = nullptr;

		for(INTN i = 0; i < MAX_TASK; i++){
			Task *T = TaskArray[i];
			if(!T) continue;
			if(T->pid == pid){
				Foundtask = T;
				break;
			}
		}
		return Foundtask;
	}

	Task *GetTaskPGID(U64 pid){
		Task *Foundtask = nullptr;

		for(INTN i = 0; i < MAX_TASK; i++){
			Task *T = TaskArray[i];
			if(!T) continue;
			if(T->PGID == pid){
				Foundtask = T;
				break;
			}
		}
		return Foundtask;
	}
}