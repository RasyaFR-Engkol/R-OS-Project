#include <rossys.hpp>
#include <rosval.h>
#include <userland/syscall.hpp>
#include "task.hpp"

namespace Tasking {
	Task *TaskArray[MAX_TASK];
	U64 ActiveTask = 0;
	U64 CurrentTaskIndex = 0;
}