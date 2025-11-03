#include <locale.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// 프로세스 상태 정의
#define READY       0   // 준비 상태
#define RUNNING     1   // 실행 중인 상태
#define WAITING     2   // 인터럽트를 받아 기다리는 상태
#define TERMINATED  3   // 종료된 상태

// 스케줄링 알고리즘
#define FCFS        0   // First-Come-First-Served
#define SJF_NP      1   // Non-Preemptive SJF
#define SJF_P       2   // Preemptive SJF
#define PRIORITY_NP 3   // Non-Preemptive Priority
#define PRIORITY_P  4   // Preemptive Priority
#define RR          5   // Round Robin

// 기타 상수들
#define QUEUE_SIZE          100 // Waiting, Ready 큐 크기
#define MAX_CPU_BURST       15  // 최대 CPU BURST 시간
#define MAX_IO_BURST        3   // 최대 IO BURST 시간
#define TIME_QUANTUM        2   // RR에서 사용할 TIME QUANTUM

// 프로세스 구조체
typedef struct
{
    // CPU 관련 속성
    int pid;                   // 프로세스 ID
    int arrival_time;          // 도착 시간
    int cpu_burst_time;        // 필요 CPU 실행 시간
    int remaining_cpu_burst;   // 남은 실행 시간
    int priority;              // 우선순위 (낮은 값이 높은 우선순위)
    int waiting_time;          // 대기 시간
    int turnaround_time;       // 반환 시간
    int state;                 // 프로세스 상태

    // I/O 관련 속성
    int *io_burst_time;        // 각 I/O 요청별 실행 시간
    int *io_request_time;      // 각 I/O 요청 시점
    int io_count;              // 총 I/O 요청 횟수
    int cur_io_index;          // 현재 처리 중인 I/O 요청 인덱스
    int remaining_io_burst;    // 현재 I/O 작업의 남은 시간
    bool io_requested;         // 현재 I/O 요청 여부
} Process;

// 큐 구조체 (원형 큐)
typedef struct
{
    Process *processes;
    int front, rear;
    int capacity;
    int size;
} Queue;

// 간트 차트 기록용
typedef struct
{
    int time;
    int running_pid;  // -1이면 IDLE
} GanttInfo;

// 시스템 구성 구조체
typedef struct
{
    Process *processes;   // 모든 프로세스
    Queue ready_queue;    // 준비 큐 (원형)
    Queue waiting_queue;  // 대기 큐 (원형)

    int process_count;    // 생성할 총 프로세스 수
    int cur_time;         // 현재 시간
    int algorithm;        // 사용 중인 스케줄링 알고리즘
    int time_quantum;     // RR 알고리즘의 시간 할당량
    int cur_used_quantum;       // RR 알고리즘에서 사용

    GanttInfo *gantt;     // 간트 차트 배열
    int gantt_index;      // 현재 간트 차트 인덱스
    int gantt_capacity;   // 간트 차트 배열 크기
} System;

// I/O 정보를 짝을 지어 정렬해도 함께 정렬되게 하기 위한 구조체
typedef struct
{
    int request_time;
    int burst_time;
} IOPair;

// I/O Request Time을 기준으로 정렬하는 함수 (퀵소트에 사용됨)
int compare_io_request(const void *a, const void *b)
{
    return ((IOPair *)a)->request_time - ((IOPair *)b)->request_time;
}

// 큐 초기화
Queue InitQueue(int capacity)
{
    Queue queue;
    queue.processes = (Process*)malloc(capacity * sizeof(Process));
    queue.front = queue.rear = -1;
    queue.capacity = capacity;      // 전체 큐 용량
    queue.size = 0;                 // 현재 큐 크기
    return queue;
}

// 큐가 비어있는지 확인
bool IsEmpty(Queue *queue)
{
    return queue->size == 0;
}

// 큐가 가득 찼는지 확인
bool IsFull(Queue *queue)
{
    return queue->size == queue->capacity;
}

// 큐에 프로세스 추가
void Enqueue(Queue *queue, Process process)
{
    if (IsFull(queue)) return;
    if (IsEmpty(queue)) queue->front = 0;

    queue->rear = (queue->rear + 1) % queue->capacity;
    queue->processes[queue->rear] = process; // rear에 새 프로세스 추가
    queue->size += 1;
}

// 큐에서 프로세스 제거
Process Dequeue(Queue *queue)
{
    Process process = {0}; // 모든 필드를 0으로 초기화

    if (IsEmpty(queue))
    {
        process.pid = -1;
        return process; // pid가 -1인 프로세스 반환 (에러 프로세스)
    }

    process = queue->processes[queue->front]; // front에 있는 프로세스 꺼내기

    // 큐에 하나의 프로세스만 존재할 때
    if (queue->front == queue->rear)
    {
        queue->front = queue->rear = -1; // 큐 전부 비우기
    }
    else
    {
        queue->front = (queue->front + 1) % queue->capacity;
    }

    queue->size -= 1;
    return process; // 제거한 큐 반환하기
}

// I/O 요청이 있는지 확인하는 함수
bool HasIORequest(Process *process)
{
    // I/O 요청이 없는 경우
    if (process->io_count == 0) return false;

    // 프로세스가 지금까지 얼마나 진행되었는지
    int running_time = process->cpu_burst_time - process->remaining_cpu_burst;

    // 남은 I/O 요청이 존재 && 현재 실행 시간이 I/O 요청 시간과 같아지면 I/O작업 해야함
    if (process->cur_io_index < process->io_count &&
         running_time == process->io_request_time[process->cur_io_index])
    {
        return true;
    }
    return false;
}

// CPU 실행 및 종료 처리 함수
void ExecuteCPU(Process *process, System *system)
{
    process->remaining_cpu_burst -= 1;
    if (process->remaining_cpu_burst == 0)
    {
        process->state = TERMINATED;
        process->turnaround_time = system->cur_time - process->arrival_time + 1;
        process->waiting_time = process->turnaround_time - process->cpu_burst_time;
    }
}

// I/O 작업 처리 함수
void HandleIO(System *system)
{
    if (IsEmpty(&system->waiting_queue))
    {
        return;
    }

    // 맨 앞의 프로세스 하나만 처리
    Process cur_waiting_process = Dequeue(&system->waiting_queue);
    cur_waiting_process.remaining_io_burst -= 1;

    // I/O 작업이 끝난 경우
    if (cur_waiting_process.remaining_io_burst <= 0)
    {
        system->processes[cur_waiting_process.pid - 1].cur_io_index += 1;
        system->processes[cur_waiting_process.pid - 1].io_requested = false;
        system->processes[cur_waiting_process.pid - 1].state = READY;
        Enqueue(&system->ready_queue, system->processes[cur_waiting_process.pid - 1]);
    }
    else
    {
        // I/O 계속 진행 - 다시 Waiting Queue 맨 뒤로
        system->processes[cur_waiting_process.pid - 1].remaining_io_burst = cur_waiting_process.remaining_io_burst;
        Enqueue(&system->waiting_queue, cur_waiting_process);
    }
}

void MakeProcess(System *system)
{
    // 모든 프로세스 만들기
    system->processes = (Process*)malloc(system->process_count * sizeof(Process));

    printf("\n[Create Process]\n");
    printf("PID\tArrival Time\tCPU Burst\tPriority\t(I/O Request Time/Burst)\n");
    printf("-------------------------------------------------------------------------\n");

    srand(time(NULL)); // 랜덤을 더 다양하게

    // 각 프로세스 생성
    for (int i = 0; i < system->process_count; i++)
    {
        Process *p = &system->processes[i];

        // 초기화
        p->pid = i + 1;
        p->arrival_time = rand() % (system->process_count * MAX_CPU_BURST / 3) + 1; // 적당히 랜덤하도록 만들기
        p->cpu_burst_time = rand() % MAX_CPU_BURST + 1;
        p->remaining_cpu_burst = p->cpu_burst_time;
        p->priority = rand() % system->process_count + 1;  // 낮은 값이 높은 우선순위
        p->waiting_time = 0;
        p->turnaround_time = 0;
        p->state = READY;

        // I/O 관련 초기화
        p->cur_io_index = 0;
        p->remaining_io_burst = 0;
        p->io_requested = false;
        p->io_count = 0; // 우선 0으로 초기화

        // CPU 버스트 시간이 1보다 작으면 I/O 요청 안됨
        if (p->cpu_burst_time <= 1)
        {
            p->io_count = 0;
            p->io_burst_time = NULL;
            p->io_request_time = NULL;
        }
        else
        {
            // CPU 버스트 크기에 비례하게 최대 I/O 요청 수 다르게 적용 (버스트 시간이 크면 요청도 많이 가능)
            int max_io_request;
            if (p->cpu_burst_time <= 1 * MAX_CPU_BURST / 3)
            {
                max_io_request = p->cpu_burst_time / 10;
            }
            else if (p->cpu_burst_time <= 2 * MAX_CPU_BURST / 3)
            {
                max_io_request = 2 * p->cpu_burst_time / 10;
            }
            else
            {
                max_io_request = 3 * p->cpu_burst_time / 10;
            }

            // 랜덤 개수의 I/O 요청 생성
            p->io_count = rand() % (max_io_request + 1);

            // I/O 요청이 있는 경우
            if (p->io_count > 0)
            {
                p->io_burst_time = (int *)malloc(p->io_count * sizeof(int));
                p->io_request_time = (int *)malloc(p->io_count * sizeof(int));

                IOPair *io_pairs = malloc(p->io_count * sizeof(IOPair));

                // I/O 요청 시간 생성
                for (int j = 0; j < p->io_count; )
                {
                    int request_time = rand() % (p->cpu_burst_time - 1) + 1;
                    bool is_duplicate = false;

                    for (int k = 0; k < j; k++)
                    {
                        // 이미 존재하는 request time인지 확인
                        if (io_pairs[k].request_time == request_time)
                        {
                            is_duplicate = true;
                            break;
                        }
                    }

                    // 중복인 경우는 j값 그대로 다시 반복
                    if (is_duplicate) continue;

                    io_pairs[j].request_time = request_time;
                    io_pairs[j].burst_time = rand() % MAX_IO_BURST + 1;
                    j += 1;
                }

                // request_time 기준으로 정렬 (burst_time도 같이 정렬됨)
                qsort(io_pairs, p->io_count, sizeof(IOPair), compare_io_request);

                // 정렬된 결과를 프로세스 배열에 복사
                for (int j = 0; j < p->io_count; j++)
                {
                    p->io_request_time[j] = io_pairs[j].request_time;
                    p->io_burst_time[j] = io_pairs[j].burst_time;
                }

                free(io_pairs);
            }
            else
            {
                // I/O 요청이 없는 경우
                p->io_burst_time = NULL;
                p->io_request_time = NULL;
            }
        }

        // 프로세스 정보 출력
        printf("P%d\t%d\t\t%d\t\t%d\t\t", p->pid, p->arrival_time, p->cpu_burst_time, p->priority);

        // I/O 요청 정보 출력
        if (p->io_count == 0)
        {
            printf("NULL");
        }
        else
        {
            for (int j = 0; j < p->io_count; j++)
            {
                printf("(%d/%d) ", p->io_request_time[j], p->io_burst_time[j]);
            }
        }
        printf("\n");
    }
    printf("-------------------------------------------------------------------------\n");
}

// 시스템 환경 설정 함수
void SysConfig(System *system)
{
    printf("\n[System Config]\n");

    printf("Input Process Count: ");
    scanf("%d", &system->process_count);

    // 초기화
    system->algorithm = FCFS;  // 기본 알고리즘 : FCFS로
    system->time_quantum = TIME_QUANTUM;

    system->ready_queue = InitQueue(system->process_count * QUEUE_SIZE);
    system->waiting_queue = InitQueue(system->process_count * QUEUE_SIZE);

    system->cur_time = 0;
    system->cur_used_quantum = 0;

    MakeProcess(system);
}

// 현재 시간에 도착한 프로세스를 Ready Queue에 추가하고 실행 중인 프로세스를 찾는 함수
Process* InitCurRunningProcess(System *system, bool *is_running)
{
    int cur_time = system->cur_time;
    Process *cur_running_process = NULL;

    // 현재 시간에 도착한 프로세스가 준비 상태로 있으면 ready queue에 추가
    for (int i = 0; i < system->process_count; i++)
    {
        Process *p = &system->processes[i];
        if (p->arrival_time == cur_time && p->state == READY)
        {
            // 이미 ready queue에 있는 프로세스면 추가 안하기
            bool is_in_queue = false;
            for (int j = 0; j < system->ready_queue.size; j++)
            {
                int idx = (system->ready_queue.front + j) % system->ready_queue.capacity;
                if (system->ready_queue.processes[idx].pid == p->pid)
                {
                    is_in_queue = true;
                    break;
                }
            }
            if (!is_in_queue)
            {
                Enqueue(&system->ready_queue, *p);
            }
        }
    }


    // 실행 중인 프로세스 찾기
    *is_running = false;
    for (int i = 0; i < system->process_count; i++)
    {
        if (system->processes[i].state == RUNNING)
        {
            *is_running = true;
            cur_running_process = &system->processes[i];
            break;
        }
    }

    return cur_running_process;
}

// 간트 차트에 현재 상태 기록
void RecordGantt(System *system, int running_pid)
{
    if (system->gantt_index < system->gantt_capacity)
    {
        system->gantt[system->gantt_index].time = system->cur_time;
        system->gantt[system->gantt_index].running_pid = running_pid;
        system->gantt_index++;
    }
}

//----------------------------------------------------------------------------------------------------//

void Fcfs(System *system)
{
    bool is_running = false;
    Process *cur_running_process = InitCurRunningProcess(system, &is_running);

    // 실행 중인 프로세스가 없지만 Ready Queue에는 프로세스가 있는 경우
    if (!is_running && !IsEmpty(&system->ready_queue))
    {
        // Ready Queue에서 도착한 프로세스 중 아무거나 선택
        Process next_process = Dequeue(&system->ready_queue);
        cur_running_process = &system->processes[next_process.pid - 1];
        system->processes[next_process.pid - 1].state = RUNNING;
    }

    // 실행 중인 프로세스를 찾은 경우
    if (cur_running_process != NULL)
    {

        // I/O 요청이 있을 경우
        if (HasIORequest(cur_running_process))
        {
            cur_running_process->state = WAITING;
            cur_running_process->io_requested = true;
            cur_running_process->remaining_io_burst = cur_running_process->io_burst_time[cur_running_process->cur_io_index];
            Enqueue(&system->waiting_queue, *cur_running_process);

            // 기존 실행 중인 프로세스가  I/O 작업하러 갔으므로 바로 다음 프로세스가 CPU 사용 가능
            if (!IsEmpty(&system->ready_queue))
            {
                Process next_process = Dequeue(&system->ready_queue);
                cur_running_process = &system->processes[next_process.pid - 1];
                system->processes[next_process.pid - 1].state = RUNNING;

                // 새 프로세스도 한 차례 실행
                ExecuteCPU(cur_running_process, system);
            }
            else
            {
                cur_running_process = NULL;
            }
        }
        else
        {
            ExecuteCPU(cur_running_process, system);
        }
        if (cur_running_process != NULL)
        {
            RecordGantt(system, cur_running_process->pid);
        }
        else
        {
            RecordGantt(system, -1);
        }
    }
    else
    {
        RecordGantt(system, -1);
    }
    HandleIO(system); // I/O 작업 처리
    system->cur_time += 1;
}

void SjfNP(System *system)
{
    bool is_running = false;
    Process *cur_running_process = InitCurRunningProcess(system, &is_running);

    // 실행 중인 프로세스가 없지만 Ready Queue에는 프로세스가 있는 경우
    if (!is_running && !IsEmpty(&system->ready_queue))
    {
        // Ready Queue에서 remaining burst time이 가장 짧은 작업 선택하기
        int short_idx = system->ready_queue.front;
        int short_burst = system->ready_queue.processes[short_idx].remaining_cpu_burst;

        for (int i = 0; i < system->ready_queue.size; i++)
        {
            int idx = (system->ready_queue.front + i) % system->ready_queue.capacity;
            if (system->ready_queue.processes[idx].remaining_cpu_burst < short_burst)
            {
                short_idx = idx;
                short_burst = system->ready_queue.processes[idx].remaining_cpu_burst;
            }
        }

        // Ready Queue에서 shortest 프로세스 가져오기 (Dequeue와 비슷함)
        Process short_process = system->ready_queue.processes[short_idx];
        Queue temp_queue = InitQueue(system->ready_queue.capacity);

        while (!IsEmpty(&system->ready_queue))
        {
            Process p = Dequeue(&system->ready_queue);
            // pid로 구분 (shortest 프로세스를 제외하고 다 임시 큐에 넣기)
            if (p.pid != short_process.pid)
            {
                Enqueue(&temp_queue, p);
            }
        }

        // 임시 큐에서 다시 ready queue로 복사
        while (!IsEmpty(&temp_queue))
        {
            Process p = Dequeue(&temp_queue);
            Enqueue(&system->ready_queue, p);
        }

        system->processes[short_process.pid - 1].state = RUNNING;
        cur_running_process = &system->processes[short_process.pid - 1];
        free(temp_queue.processes);
    }

    // 실행 중인 프로세스를 찾은 경우
    if (cur_running_process != NULL)
    {
        // I/O 요청이 있을 경우
        if (HasIORequest(cur_running_process))
        {
            cur_running_process->state = WAITING;
            cur_running_process->io_requested = true;
            cur_running_process->remaining_io_burst = cur_running_process->io_burst_time[cur_running_process->cur_io_index];
            Enqueue(&system->waiting_queue, *cur_running_process);

            // 즉시 다음 프로세스 선택 (SJF 방식)
            if (!IsEmpty(&system->ready_queue))
            {
                int short_idx = system->ready_queue.front;
                int short_burst = system->ready_queue.processes[short_idx].remaining_cpu_burst;

                for (int i = 0; i < system->ready_queue.size; i++)
                {
                    int idx = (system->ready_queue.front + i) % system->ready_queue.capacity;
                    if (system->ready_queue.processes[idx].remaining_cpu_burst < short_burst)
                    {
                        short_idx = idx;
                        short_burst = system->ready_queue.processes[idx].remaining_cpu_burst;
                    }
                }

                Process short_process = system->ready_queue.processes[short_idx];
                Queue temp_queue = InitQueue(system->ready_queue.capacity);

                while (!IsEmpty(&system->ready_queue))
                {
                    Process p = Dequeue(&system->ready_queue);
                    if (p.pid != short_process.pid)
                    {
                        Enqueue(&temp_queue, p);
                    }
                }

                while (!IsEmpty(&temp_queue))
                {
                    Process p = Dequeue(&temp_queue);
                    Enqueue(&system->ready_queue, p);
                }

                cur_running_process = &system->processes[short_process.pid - 1];
                system->processes[short_process.pid - 1].state = RUNNING;

                // 새 프로세스도 한 차례 실행
                ExecuteCPU(cur_running_process, system);
                free(temp_queue.processes);
            }
            else
            {
                cur_running_process = NULL;
            }
        }
        else
        {
            ExecuteCPU(cur_running_process, system);
        }
        if (cur_running_process != NULL)
        {
            RecordGantt(system, cur_running_process->pid);
        }
        else
        {
            RecordGantt(system, -1);
        }
    }
    else
    {
        RecordGantt(system, -1);
    }
    HandleIO(system);
    system->cur_time += 1;
}

void SjfP(System *system)
{
    bool is_running = false;
    Process *cur_running_process = InitCurRunningProcess(system, &is_running);

    // 실행 중인 프로세스가 없지만 Ready Queue에는 프로세스가 있는 경우
    if (!is_running && !IsEmpty(&system->ready_queue))
    {
        // Ready Queue에서 remaining burst time이 가장 짧은 작업 선택하기
        int shortest_idx = system->ready_queue.front;
        int shortest_burst = system->ready_queue.processes[shortest_idx].remaining_cpu_burst;

        for (int i = 0; i < system->ready_queue.size; i++)
        {
            int idx = (system->ready_queue.front + i) % system->ready_queue.capacity;
            if (system->ready_queue.processes[idx].remaining_cpu_burst < shortest_burst)
            {
                shortest_idx = idx;
                shortest_burst = system->ready_queue.processes[idx].remaining_cpu_burst;
            }
        }

        // Ready Queue에서 shortest 프로세스 가져오기
        Process shortest_process = system->ready_queue.processes[shortest_idx];
        Queue temp_queue = InitQueue(system->ready_queue.capacity);

        while (!IsEmpty(&system->ready_queue))
        {
            Process p = Dequeue(&system->ready_queue);
            if (p.pid != shortest_process.pid)
            {
                Enqueue(&temp_queue, p);
            }
        }

        while (!IsEmpty(&temp_queue))
        {
            Process p = Dequeue(&temp_queue);
            Enqueue(&system->ready_queue, p);
        }

        system->processes[shortest_process.pid - 1].state = RUNNING;
        cur_running_process = &system->processes[shortest_process.pid - 1];
        free(temp_queue.processes);
    }

    // 실행 중인 프로세스가 있고 Ready Queue에도 프로세스가 있는 경우 선점 가능한지 확인
    if (cur_running_process != NULL)
    {
        // I/O 요청이 있을 경우
        if (HasIORequest(cur_running_process))
        {
            cur_running_process->state = WAITING;
            cur_running_process->io_requested = true;
            cur_running_process->remaining_io_burst = cur_running_process->io_burst_time[cur_running_process->cur_io_index];
            Enqueue(&system->waiting_queue, *cur_running_process);

            // I/O로 인해 CPU가 비었으므로 Ready Queue에서 다음 프로세스 선택
            if (!IsEmpty(&system->ready_queue))
            {
                int shortest_idx = system->ready_queue.front;
                int shortest_burst = system->ready_queue.processes[shortest_idx].remaining_cpu_burst;

                for (int i = 0; i < system->ready_queue.size; i++)
                {
                    int idx = (system->ready_queue.front + i) % system->ready_queue.capacity;
                    if (system->ready_queue.processes[idx].remaining_cpu_burst < shortest_burst)
                    {
                        shortest_idx = idx;
                        shortest_burst = system->ready_queue.processes[idx].remaining_cpu_burst;
                    }
                }

                Process shortest_process = system->ready_queue.processes[shortest_idx];
                Queue temp_queue = InitQueue(system->ready_queue.capacity);

                while (!IsEmpty(&system->ready_queue))
                {
                    Process p = Dequeue(&system->ready_queue);
                    if (p.pid != shortest_process.pid)
                    {
                        Enqueue(&temp_queue, p);
                    }
                }

                while (!IsEmpty(&temp_queue))
                {
                    Process p = Dequeue(&temp_queue);
                    Enqueue(&system->ready_queue, p);
                }

                cur_running_process = &system->processes[shortest_process.pid - 1];
                system->processes[shortest_process.pid - 1].state = RUNNING;

                // 새 프로세스도 한 차례 실행
                ExecuteCPU(cur_running_process, system);
                free(temp_queue.processes);
            }
            else
            {
                cur_running_process = NULL; // CPU가 idle 상태
            }
        }
        // I/O 요청이 없을 경우
        else if (is_running && !IsEmpty(&system->ready_queue))
        {
            int shortest_idx = -1;
            int shortest_burst = cur_running_process->remaining_cpu_burst;

            for (int i = 0; i < system->ready_queue.size; i++)
            {
                int idx = (system->ready_queue.front + i) % system->ready_queue.capacity;
                if (system->ready_queue.processes[idx].remaining_cpu_burst < shortest_burst)
                {
                    shortest_idx = idx;
                    shortest_burst = system->ready_queue.processes[idx].remaining_cpu_burst;
                }
            }

            // 더 짧은 프로세스가 있는 경우 선점
            if (shortest_idx != -1)
            {
                Process shortest_process = system->ready_queue.processes[shortest_idx];

                cur_running_process->state = READY;
                Enqueue(&system->ready_queue, *cur_running_process);

                Queue temp_queue = InitQueue(system->ready_queue.capacity);
                while (!IsEmpty(&system->ready_queue))
                {
                    Process p = Dequeue(&system->ready_queue);
                    if (p.pid != shortest_process.pid)
                    {
                        Enqueue(&temp_queue, p);
                    }
                }
                while (!IsEmpty(&temp_queue))
                {
                    Process p = Dequeue(&temp_queue);
                    Enqueue(&system->ready_queue, p);
                }

                system->processes[shortest_process.pid - 1].state = RUNNING;
                cur_running_process = &system->processes[shortest_process.pid - 1];
                free(temp_queue.processes);

                ExecuteCPU(cur_running_process, system);
            }
            else
            {
                ExecuteCPU(cur_running_process, system);
            }
        }
        else
        {
            ExecuteCPU(cur_running_process, system);
        }
    }

    if (cur_running_process != NULL)
    {
        RecordGantt(system, cur_running_process->pid);
    }
    else
    {
        RecordGantt(system, -1);
    }

    HandleIO(system);
    system->cur_time += 1;
}

void priority_np(System *system)
{
    bool is_running = false;
    Process *cur_running_process = InitCurRunningProcess(system, &is_running);

    // 실행 중인 프로세스가 없지만 Ready Queue에는 프로세스가 있는 경우
    if (!is_running && !IsEmpty(&system->ready_queue))
    {
        // 가장 높은 우선순위(작은 값)를 가진 프로세스 선택 (SJF에서 burst 대신 priority 사용하면 됨)
        int highest_idx = system->ready_queue.front;
        int highest_priority = system->ready_queue.processes[highest_idx].priority;

        for (int i = 0; i < system->ready_queue.size; i++)
        {
            int idx = (system->ready_queue.front + i) % system->ready_queue.capacity;
            if (system->ready_queue.processes[idx].priority < highest_priority)
            {
                highest_idx = idx;
                highest_priority = system->ready_queue.processes[idx].priority;
            }
        }

        Process highest_process = system->ready_queue.processes[highest_idx];
        Queue temp_queue = InitQueue(system->ready_queue.capacity);

        while (!IsEmpty(&system->ready_queue))
        {
            Process p = Dequeue(&system->ready_queue);
            if (p.pid != highest_process.pid)
            {
                Enqueue(&temp_queue, p);
            }
        }
        while (!IsEmpty(&temp_queue))
        {
            Process p = Dequeue(&temp_queue);
            Enqueue(&system->ready_queue, p);
        }

        system->processes[highest_process.pid - 1].state = RUNNING;
        cur_running_process = &system->processes[highest_process.pid - 1];
        free(temp_queue.processes);
    }

    // 현재 실행 중인 프로세스가 있는 경우
    if (cur_running_process != NULL)
    {
        // I/O 요청이 있을 경우
        if (HasIORequest(cur_running_process))
        {
            cur_running_process->state = WAITING;
            cur_running_process->io_requested = true;
            cur_running_process->remaining_io_burst = cur_running_process->io_burst_time[cur_running_process->cur_io_index];
            Enqueue(&system->waiting_queue, *cur_running_process);

            // I/O로 인해 CPU가 비었으므로 Ready Queue에서 다음 프로세스 선택
            if (!IsEmpty(&system->ready_queue))
            {
                int highest_idx = system->ready_queue.front;
                int highest_priority = system->ready_queue.processes[highest_idx].priority;

                for (int i = 0; i < system->ready_queue.size; i++)
                {
                    int idx = (system->ready_queue.front + i) % system->ready_queue.capacity;
                    if (system->ready_queue.processes[idx].priority < highest_priority)
                    {
                        highest_idx = idx;
                        highest_priority = system->ready_queue.processes[idx].priority;
                    }
                }

                Process highest_process = system->ready_queue.processes[highest_idx];
                Queue temp_queue = InitQueue(system->ready_queue.capacity);

                while (!IsEmpty(&system->ready_queue))
                {
                    Process p = Dequeue(&system->ready_queue);
                    if (p.pid != highest_process.pid)
                    {
                        Enqueue(&temp_queue, p);
                    }
                }

                while (!IsEmpty(&temp_queue))
                {
                    Process p = Dequeue(&temp_queue);
                    Enqueue(&system->ready_queue, p);
                }

                cur_running_process = &system->processes[highest_process.pid - 1];
                system->processes[highest_process.pid - 1].state = RUNNING;

                // 새 프로세스도 한 차례 실행
                ExecuteCPU(cur_running_process, system);
                free(temp_queue.processes);
            }
            else
            {
                cur_running_process = NULL;
            }
        }
        else
        {
            ExecuteCPU(cur_running_process, system);
        }
        if (cur_running_process != NULL)
        {
            RecordGantt(system, cur_running_process->pid);
        }
        else
        {
            RecordGantt(system, -1);
        }
    }
    else
    {
        RecordGantt(system, -1);
    }
    HandleIO(system);
    system->cur_time += 1;
}

void priority_p(System *system)
{
    bool is_running = false;
    Process *cur_running_process = InitCurRunningProcess(system, &is_running);

    // 실행 중인 프로세스가 없지만 Ready Queue에는 프로세스가 있는 경우
    if (!is_running && !IsEmpty(&system->ready_queue))
    {
        // Ready Queue에서 우선순위가 가장 높은 작업 선택하기
        int highest_idx = system->ready_queue.front;
        int highest_priority = system->ready_queue.processes[highest_idx].priority;

        for (int i = 0; i < system->ready_queue.size; i++)
        {
            int idx = (system->ready_queue.front + i) % system->ready_queue.capacity;
            if (system->ready_queue.processes[idx].priority < highest_priority)
            {
                highest_idx = idx;
                highest_priority = system->ready_queue.processes[idx].priority;
            }
        }

        Process highest_process = system->ready_queue.processes[highest_idx];
        Queue temp_queue = InitQueue(system->ready_queue.capacity);

        while (!IsEmpty(&system->ready_queue))
        {
            Process p = Dequeue(&system->ready_queue);
            if (p.pid != highest_process.pid)
            {
                Enqueue(&temp_queue, p);
            }
        }
        while (!IsEmpty(&temp_queue))
        {
            Process p = Dequeue(&temp_queue);
            Enqueue(&system->ready_queue, p);
        }

        system->processes[highest_process.pid - 1].state = RUNNING;
        cur_running_process = &system->processes[highest_process.pid - 1];
        free(temp_queue.processes);
    }

    // 실행 중인 프로세스가 있고 Ready Queue에도 프로세스가 있는 경우 선점 가능한지 확인
    if (cur_running_process != NULL)
    {
        // I/O 요청이 있을 경우
        if (HasIORequest(cur_running_process))
        {
            cur_running_process->state = WAITING;
            cur_running_process->io_requested = true;
            cur_running_process->remaining_io_burst = cur_running_process->io_burst_time[cur_running_process->cur_io_index];
            Enqueue(&system->waiting_queue, *cur_running_process);

            // I/O로 인해 CPU가 비었으므로 Ready Queue에서 다음 프로세스 선택
            if (!IsEmpty(&system->ready_queue))
            {
                int highest_idx = system->ready_queue.front;
                int highest_priority = system->ready_queue.processes[highest_idx].priority;

                for (int i = 0; i < system->ready_queue.size; i++)
                {
                    int idx = (system->ready_queue.front + i) % system->ready_queue.capacity;
                    if (system->ready_queue.processes[idx].priority < highest_priority)
                    {
                        highest_idx = idx;
                        highest_priority = system->ready_queue.processes[idx].priority;
                    }
                }

                Process highest_process = system->ready_queue.processes[highest_idx];
                Queue temp_queue = InitQueue(system->ready_queue.capacity);

                while (!IsEmpty(&system->ready_queue))
                {
                    Process p = Dequeue(&system->ready_queue);
                    if (p.pid != highest_process.pid)
                    {
                        Enqueue(&temp_queue, p);
                    }
                }

                while (!IsEmpty(&temp_queue))
                {
                    Process p = Dequeue(&temp_queue);
                    Enqueue(&system->ready_queue, p);
                }

                cur_running_process = &system->processes[highest_process.pid - 1];
                system->processes[highest_process.pid - 1].state = RUNNING;

                // 새 프로세스도 한 차례 실행
                ExecuteCPU(cur_running_process, system);
                free(temp_queue.processes);
            }
            else
            {
                cur_running_process = NULL; // CPU가 idle 상태
            }
        }
        else
        {
            if (is_running && !IsEmpty(&system->ready_queue))
            {
                int highest_idx = -1;
                int highest_priority = cur_running_process->priority;

                for (int i = 0; i < system->ready_queue.size; i++)
                {
                    int idx = (system->ready_queue.front + i) % system->ready_queue.capacity;
                    if (system->ready_queue.processes[idx].priority < highest_priority)
                    {
                        highest_idx = idx;
                        highest_priority = system->ready_queue.processes[idx].priority;
                    }
                }

                // 더 높은 우선순위 프로세스가 있으면 선점
                if (highest_idx != -1)
                {
                    Process highest_process = system->ready_queue.processes[highest_idx];

                    cur_running_process->state = READY;
                    Enqueue(&system->ready_queue, *cur_running_process);

                    Queue temp_queue = InitQueue(system->ready_queue.capacity);
                    while (!IsEmpty(&system->ready_queue))
                    {
                        Process p = Dequeue(&system->ready_queue);
                        if (p.pid != highest_process.pid)
                        {
                            Enqueue(&temp_queue, p);
                        }
                    }
                    while (!IsEmpty(&temp_queue))
                    {
                        Process p = Dequeue(&temp_queue);
                        Enqueue(&system->ready_queue, p);
                    }

                    system->processes[highest_process.pid - 1].state = RUNNING;
                    cur_running_process = &system->processes[highest_process.pid - 1];
                    free(temp_queue.processes);
                }
            }

            // CPU 실행
            ExecuteCPU(cur_running_process, system);
        }
    }

    if (cur_running_process != NULL)
    {
        RecordGantt(system, cur_running_process->pid);
    }
    else
    {
        RecordGantt(system, -1);
    }
    HandleIO(system);
    system->cur_time += 1;
}

void round_robin(System *system)
{
    bool is_running = false;
    Process *cur_running_process = InitCurRunningProcess(system, &is_running);

    // 실행 중인 프로세스가 없지만 Ready Queue에는 프로세스가 있는 경우
    if (!is_running && !IsEmpty(&system->ready_queue))
    {
        // ready queue에서 다음 프로세스 선택
        Process next_process = Dequeue(&system->ready_queue);

        system->processes[next_process.pid - 1].state = RUNNING;
        cur_running_process = &system->processes[next_process.pid - 1];
        system->cur_used_quantum = 0;
    }

    // 현재 실행 중인 프로세스가 있는 경우
    if (cur_running_process != NULL)
    {
        // I/O 요청이 있을 경우
        if (HasIORequest(cur_running_process))
        {
            cur_running_process->state = WAITING;
            cur_running_process->io_requested = true;
            cur_running_process->remaining_io_burst = cur_running_process->io_burst_time[cur_running_process->cur_io_index];
            Enqueue(&system->waiting_queue, *cur_running_process);
            system->cur_used_quantum = 0;

            // I/O로 인해 CPU가 비었으므로 Ready Queue에서 다음 프로세스 선택
            if (!IsEmpty(&system->ready_queue))
            {
                Process next_process = Dequeue(&system->ready_queue);
                cur_running_process = &system->processes[next_process.pid - 1];
                system->processes[next_process.pid - 1].state = RUNNING;
                system->cur_used_quantum = 0;

                // 새 프로세스도 한 차례(1초) 실행
                ExecuteCPU(cur_running_process, system);
                system->cur_used_quantum += 1;

                if (cur_running_process->state == TERMINATED)
                {
                    system->cur_used_quantum = 0;
                }
            }
            else
            {
                cur_running_process = NULL; // 준비 큐에 아무 프로세스도 없을 경우 CPU가 idle 상태
            }
        }
        // 할당된 시간을 다 쓴 경우
        else if (is_running && system->cur_used_quantum >= system->time_quantum)
        {
            // 현재 실행 중인 프로세스를 ready queue로 돌려놓음
            cur_running_process->state = READY;
            Enqueue(&system->ready_queue, *cur_running_process);

            // I/O로 인해 CPU가 비었으므로 Ready Queue에서 다음 프로세스 선택
            if (!IsEmpty(&system->ready_queue))
            {
                Process next_process = Dequeue(&system->ready_queue);
                cur_running_process = &system->processes[next_process.pid - 1];
                system->processes[next_process.pid - 1].state = RUNNING;
                system->cur_used_quantum = 0;

                // 새 프로세스도 한 차례 실행
                ExecuteCPU(cur_running_process, system);
                system->cur_used_quantum += 1;

                if (cur_running_process->state == TERMINATED)
                {
                    system->cur_used_quantum = 0;
                }
            }
            else
            {
                cur_running_process = NULL; // CPU가 idle 상태
                system->cur_used_quantum = 0;
            }
        }
        else
        {
            // 일반적인 CPU 실행
            ExecuteCPU(cur_running_process, system);
            system->cur_used_quantum += 1;

            if (cur_running_process->state == TERMINATED)
            {
                system->cur_used_quantum = 0;
            }
        }
    }

    if (cur_running_process != NULL)
    {
        RecordGantt(system, cur_running_process->pid);
    }
    else
    {
        RecordGantt(system, -1);
    }

    HandleIO(system);
    system->cur_time += 1;
}

bool IsAllProcessesTerminated(System *system)
{
    for (int i = 0; i < system->process_count; i++)
    {
        if (system->processes[i].state != TERMINATED)
        {
            return false;
        }
    }
    return true;
}

// 모든 알고리즘 실행 및 결과 출력 함수
void RunAllAlgorithms(System *system, Process *backup)
{
    char* algorithm_names[] = {"FCFS", "SJF (Non-Preemptive)", "SJF (Preemptive)", "Priority (Non-Preemptive)", "Priority (Preemptive)", "Round Robin"};

    // 모든 알고리즘 실행
    for (int alg = 0; alg <= 5; alg++)
    {
        // 간트차트 초기화
        system->gantt = (GanttInfo*)malloc(1000 * sizeof(GanttInfo));
        system->gantt_index = 0;
        system->gantt_capacity = 1000;

        // 프로세스 상태 초기화
        for (int i = 0; i < system->process_count; i++)
        {
            system->processes[i].remaining_cpu_burst = backup[i].cpu_burst_time;
            system->processes[i].waiting_time = 0;
            system->processes[i].turnaround_time = 0;
            system->processes[i].state = READY;
            system->processes[i].cur_io_index = 0;
            system->processes[i].remaining_io_burst = 0;
            system->processes[i].io_requested = false;
        }

        // 시스템 초기화
        system->algorithm = alg;
        system->cur_time = 0;
        system->cur_used_quantum = 0;

        // 큐 초기화
        while (!IsEmpty(&system->ready_queue)) Dequeue(&system->ready_queue);
        while (!IsEmpty(&system->waiting_queue)) Dequeue(&system->waiting_queue);

        // 해당 알고리즘 실행
        while (!IsAllProcessesTerminated(system))
        {
            switch (system->algorithm)
            {
                case FCFS:
                    Fcfs(system);
                    break;
                case SJF_NP:
                    SjfNP(system);
                    break;
                case SJF_P:
                    SjfP(system);
                    break;
                case PRIORITY_NP:
                    priority_np(system);
                    break;
                case PRIORITY_P:
                    priority_p(system);
                    break;
                case RR:
                    round_robin(system);
                    break;
            }
        }

        // 결과 출력
        printf("\n[%s]\n", algorithm_names[alg]);
        printf("PID\tTurnaround Time\tWaiting Time\n");
        printf("------------------------------------\n");

        double total_turnaround = 0, total_waiting = 0;
        for (int i = 0; i < system->process_count; i++)
        {
            Process *p = &system->processes[i];
            printf("P%d\t%d\t\t%d\n", p->pid, p->turnaround_time, p->waiting_time);
            total_turnaround += p->turnaround_time;
            total_waiting += p->waiting_time;
        }

        // 각종 시간 지표 출력
        printf("------------------------------------\n");
        printf("Average Turnaround Time: %.2f\n", total_turnaround / system->process_count);
        printf("Average Waiting Time: %.2f\n", total_waiting / system->process_count);

        // 간트 차트 출력
        printf("\nGantt Chart:\n");
        printf("Time: ");
        for (int i = 0; i < system->gantt_index; i++)
        {
            printf("%2d ", system->gantt[i].time);
        }
        printf("\nCPU:  ");
        for (int i = 0; i < system->gantt_index; i++)
        {
            if (system->gantt[i].running_pid == -1)
            {
                printf("-- ");
            }
            else
            {
                printf("P%d ", system->gantt[i].running_pid);
            }
        }
        printf("\n\n");

        free(system->gantt);  // 메모리 해제
    }
}

int main() {
    System sys;
    SysConfig(&sys);

    Process *backup = (Process*)malloc(sys.process_count * sizeof(Process));
    for (int i = 0; i < sys.process_count; i++)
    {
        backup[i] = sys.processes[i];  // 백업용 구조체 전체 복사
    }

    RunAllAlgorithms(&sys, backup);

    // 메모리 해제
    free(backup);
    for (int i = 0; i < sys.process_count; i++)
    {
        if (sys.processes[i].io_count > 0)
        {
            free(sys.processes[i].io_burst_time);
            free(sys.processes[i].io_request_time);
        }
    }
    free(sys.processes);
    free(sys.ready_queue.processes);
    free(sys.waiting_queue.processes);
    return 0;
}