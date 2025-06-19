#include <modules/RendezvOS_Linux/syscall_entry.h>
#include <modules/log/log.h>


void syscall(){
	return;
}
static inline void set_syscall_entry(void (*syscall_entry)(void)){
	if(!syscall_entry){
		pr_error("[Error] no syscall entry is defined\n");
		return;
	}
	return;
}
void init_syscall_entry(){
	set_syscall_entry(syscall);
}