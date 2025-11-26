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
	BOOL ForceReschedule = FALSE;

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

	Task *GetTaskPGID(U64 pgid){
		// Prefer returning the group leader (task whose pid == pgid and PGID == pgid)
		Task *leader = Tasking::GetTaskPID(pgid);
		if (leader && leader->PGID == pgid) {
			return leader;
		}

		// Fallback: return the first task whose PGID matches (keeps backward compatibility)
		for(INTN i = 0; i < MAX_TASK; i++){
			Task *T = TaskArray[i];
			if(!T) continue;
			if(T->PGID == pgid){
				return T;
			}
		}

		return nullptr;
	}


	// Deliver a signal to a single task or to all members of a process group.
	// If `isGroup` is TRUE, `id` is treated as a PGID and the function walks
	// the group's linked list via `PGIDTaskPtr`, delivering the signal to each
	// task. Signal number should be the POSIX signal number (e.g., 2 for SIGINT).
	VOID SetTaskSignal(U64 id, U32 signal, BOOL isGroup){
		LOCKRFLAGS irq = Arch::SaveAndDisableInterrupts();
		if(isGroup){
			Task *Head = GetTaskPID(id);
			Task *t = Head;
			while(t != nullptr){
				if(t->State != TaskState::ZOMBIE && t->pid > 1){
					t->Signals |= (1 << signal);
					if(t->State == TaskState::BLOCKED){
						t->State = TaskState::READY;
					}
				}
				t = t->PGIDTaskPtr;
			}
		} else {
			Task *t = GetTaskPID(id);
			if(t && t->State != TaskState::ZOMBIE && t->pid > 1){
				t->Signals |= (1 << signal);
				if(t->State == TaskState::BLOCKED){
					t->State = TaskState::READY;
				}
			}
		}
		Arch::RestoreInterrupts(irq);
	}

	// helper untuk unlink
	// process dari process group
	VOID UnlinkFromProcGrp(Task *T){
		if(!T || T->PGID == 0) return;

		Tasking::Task *Head = GetTaskPGID(T->PGID);

		if(!Head) {
			T->PGIDTaskPtr = nullptr;
			return;
		}

		if(Head == T){
			T->PGIDTaskPtr = nullptr;
			return;
		}

		Tasking::Task *Prev = Head;
		while(Prev && Prev->PGIDTaskPtr != T){
			Prev = Prev->PGIDTaskPtr;
		}

		if(Prev){
			Prev->PGIDTaskPtr = T->PGIDTaskPtr;
		}

		T->PGIDTaskPtr = nullptr;
	}

	VOID UnblockTaskWithIOBoost(Task *T){
		if(!T) return;

		T->State = TaskState::READY;

		T->Priority = 0;

		T->TimeSlice = Tasking::GetTimeSliceForPriority(0); // Reset timeslice
		T->TimeUsedInPriority = 0;

		ForceReschedule = TRUE;
	}
}