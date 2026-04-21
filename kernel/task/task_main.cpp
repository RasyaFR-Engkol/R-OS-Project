#include <rossys.hpp>
#include <rosval.h>
#include <userland/syscall.hpp>
#include "task.hpp"
#include <../firmware/acpi/driver/timer/timer.hpp>
#include <filesystem/filesystem.hpp>

namespace Tasking {
	Task *TaskArray[MAX_TASK];
	Task *GraveyardArray[MAX_TASK];
	U64 ActiveTask = 0;
	U64 CurrentTaskIndex = MAX_TASK;
	VOLATILE BOOL SchedulerActive = FALSE;
	U64 g_ForegroundPID = -1;
	VOLATILE BOOL ForceReschedule = FALSE;

	static const U64 PrioToWeight[40] = {
		/* -20 */ 88761, 71755, 56483, 46273, 36291,
		/* -15 */ 29154, 23254, 18705, 14949, 11916,
		/* -10 */  9548,  7620,  6100,  4904,  3906,
		/* -5 */  3121,  2501,  1991,  1586,  1277,
		/* 0 */  1024,   820,   655,   526,   423,
		/* 5 */   335,   272,   215,   172,   137,
		/* 10 */   110,    87,    70,    56,    45,
		/* 15 */    36,    29,    23,    18,    15
	};

	U64 RateThisTaskNice(I8 RateThisTaskNice) {
		// Safety clamp: pastikan nilai gak keluar dari -20 s/d 19 biar gak segfault
		if (RateThisTaskNice < -20) RateThisTaskNice = -20;
		if (RateThisTaskNice > 19) RateThisTaskNice = 19;
		
		// Offset +20 biar array index-nya mulai dari 0
		return PrioToWeight[RateThisTaskNice + 20];
	}

	Task *GetTaskPID(U64 pid){
		if (pid >= MAX_TASK) return nullptr;
		// O(1) array access
		Task *t = TaskArray[pid]; 
		
		// Validasi double check (opsional, tapi bagus untuk safety)
		if (t && t->pid == pid) return t;
		return nullptr;
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
					// kalo SIGINT, langsung bangunin kalo lagi BLOCKED. Priority 0
					if(t->Signals & (1 << 2)){
						t->vruntime = Tasking::CFSLeftmost->vruntime;
					}

					if(t->State == TaskState::BLOCKED){
						t->State = TaskState::READY;
						CFSEnqueue(t);
					}
				}
				t = t->PGIDTaskPtr;
			}
		} else {
			Task *t = GetTaskPID(id);
			if(t && t->State != TaskState::ZOMBIE && t->pid > 1){
				t->Signals |= (1 << signal);

				// kalo SIGINT, langsung bangunin kalo lagi BLOCKED. Priority 0
				if(t->Signals & (1 << 2)){
					t->vruntime = Tasking::CFSLeftmost->vruntime;
				}

				if(t->State == TaskState::BLOCKED){
					t->State = TaskState::READY;
					CFSEnqueue(t); 
				}
			}
		}

		ForceReschedule = TRUE;
		Arch::RestoreInterrupts(irq);
	}

	// helper untuk unlink
	// process dari process group
	VOID UnlinkFromProcGrp(Task *T){
		if(!T || T->PGID == 0) return;

		Task *Head = GetTaskPGID(T->PGID); // Asumsi ini balikin Head grup
		if(!Head) return;

		// Kasus 1: Yang mau dihapus adalah Head
		if(Head == T){
			// Lu harus update global/array mapping bahwa Head grup PGID ini 
			// sekarang adalah T->PGIDTaskPtr (node selanjutnya)
			// SetTaskPGIDHead(T->PGID, T->PGIDTaskPtr); <--- Lu butuh fungsi ginian
			
			// Untuk sekarang, minimal jangan bikin loop:
			T->PGIDTaskPtr = nullptr;
			return;
		}

		// Kasus 2: Ada di tengah/akhir
		Task *Prev = Head;
		while(Prev && Prev->PGIDTaskPtr != T){
			Prev = Prev->PGIDTaskPtr;
			// Safety break
			if (Prev == Head) break; // Circular detect
		}

		if(Prev && Prev->PGIDTaskPtr == T){
			Prev->PGIDTaskPtr = T->PGIDTaskPtr; // Bypass T
		}

		T->PGIDTaskPtr = nullptr; // Putus total
	}

	VOID UnblockTaskWithIOBoost(Task *t){
        if(!t) return;
        
        // IO BOOST versi CFS sesungguhnya: 
        // Jangan pake CFSLeftmost karena bisa NULL kalau antrian kosong.
        // Pake MinVRuntime, dan kasih "diskon" biar dia pasti menang.
        if (MinVRuntime > 10) {
            t->vruntime = MinVRuntime - 10; 
        } else {
            t->vruntime = 0;
        }

        CFSEnqueue(t);

        // MUTLAK PREEMPT! Kalau ada I/O (keyboard/mouse/disk) beres, 
        // langsung tendang CPU biar responsive.
        ForceReschedule = TRUE;
    }

	short CheckFileDesc(int fd, short events){
		Tasking::Task *Current = Tasking::GetCurrentTaskPtr();
		if(!Current) return POLLNVAL;

		if(fd < 0 || fd >= MAX_FILE_IN_PROCESS){
			return POLLNVAL;
		}

		File *files = Current->FDTable[fd];

		if(!files){
			return POLLNVAL;
		}

		return files->Poll(events);
	}

	VOID SettingAppPerm(Task *t, U32 Perm){
		if(!t || !Perm) return;

		Printk::Write(Printk::Level::LOG_INFO, "Setting permissions for PID %d: 0x%x\n", t->pid, Perm);
		
		if(Perm & PERM_ADMIN_SUDO){
			t->IsSudoOrAdmin = TRUE;
			Printk::Write(Printk::Level::LOG_INFO, "PID %d granted SUDO/ADMIN permissions.\n", t->pid);
		}
		if(Perm & PERM_ESSENTIAL_SYSTEM){
			t->IsEssentialSystem = TRUE;
			Printk::Write(Printk::Level::LOG_INFO, "PID %d marked as Essential System Process.\n", t->pid);
		}
		if(Perm & PERM_SYS_CRITICAL){
			t->IsCriticalProc = TRUE;
			Printk::Write(Printk::Level::LOG_INFO, "PID %d marked as Critical Process.\n", t->pid);
		}
	}

	BOOL BlockTaskCauseOfIOAndYield(Task *t){
		if(!t) return FALSE;

		t->State = TaskState::BLOCKED;
		t->BlockReason = TASK_WAIT_IO;

		SchedulerYield();

		return TRUE;
	}
}