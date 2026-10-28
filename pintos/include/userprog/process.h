#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

tid_t process_create_initd(const char *file_name);
tid_t process_fork(const char *name, struct intr_frame *if_);
int process_exec(void *f_name);
int process_wait(tid_t);
void process_exit(void);
void process_activate(struct thread *next);

struct fork_context
{
    struct intr_frame user_frame; /* 사용자 intr_frame 스냅샷 */
    struct semaphore done;        /* 복제 완료를 알릴 세마포어*/
    struct thread *parent;        /* 부모 스레드를 찾을 정보 */
    struct child_status *cs;      /* 기록용 포인터*/
    bool success;                 /* 복제 성공/실패 결과 */
};

struct child_status
{
    struct list_elem elem;      /* 리스트 연결용 list_elem */
    struct semaphore exit_sema; /* 종료 알림용 세마포어 */
    int ref_count;              /* 참조 카운트 */
    int pid;                    /* 자식 PID */
    int exit_status;            /* 반환값 */
};

#endif /* userprog/process.h */
