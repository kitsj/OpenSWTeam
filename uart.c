#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <wiringPi.h>
#include <softTone.h>
#include <wiringSerial.h>

#define BAUD_RATE 115200

// 설정값들
int min_time = 8;   // 최소 대기 시간 (초 단위 예제용)
int max_time = 13;   // 최대 대기 시간 (초 단위 예제용)
int first_time = 10; // 첫 복용 시간 (24시간 형식)
int max_count = 3;   // 하루 복용 횟수 제한

int today_count = 0; // 오늘 복용 횟수
int total_count = 0; // 총 복용 횟수
int flag = 0;        // 복용 가능 플래그

pthread_mutex_t flag_mutex; // 복용 가능 여부 제어용 mutex
pthread_mutex_t mid;        // 타이머 및 상태 제어용 mutex

static const char* UART2_DEV = "/dev/ttyAMA0"; // UART2
extern char** environ;

// 스텝모터 관련 GPIO 및 설정
int pin_arr[4] = { 12, 16, 20, 21 };
int one_phase[8][4] = {
    {1, 0, 0, 0},
    {1, 1, 0, 0},
    {0, 1, 0, 0},
    {0, 1, 1, 0},
    {0, 0, 1, 0},
    {0, 0, 1, 1},
    {0, 0, 0, 1},
    {1, 0, 0, 1},
};

// 문자열 전송 함수
void send_message(int fd, const char* msg) {
    while (*msg) {
        serialPutchar(fd, *msg++);
    }
    serialPutchar(fd, '\n'); // 줄바꿈 문자 추가
}

// 스텝모터 동작 함수
void one_two_Phase_Rotate_Angle(float angle, int dir) {
    int steps = angle * (4096 / 360);
    for (int i = 0; i < steps; i++) {
        for (int j = 0; j < 4; j++) {
            digitalWrite(pin_arr[j], one_phase[dir == 1 ? i % 8 : (7 - i % 8)][j]);
        }
        delay(2);
    }
}

// 음악 알림 함수
void music(int gpio) {
    softToneCreate(gpio);
    for (int i = 0; i < 3; i++) {
        softToneWrite(gpio, 900);
        delay(333);
        softToneWrite(gpio, 0);
        delay(333);
    }
}

// 타이머 스레드 함수들
void* take_min(void* arg) {
    sleep(min_time);
    pthread_mutex_lock(&mid);
    flag = 1;
    pthread_mutex_unlock(&mid);
    printf("알림: 최소 대기 시간 지나 복용 가능\n");
    return NULL;
}

void* take_max(void* arg) {
    sleep(max_time);
    pthread_mutex_lock(&mid);
    if (flag == 1) {
        printf("알림: 최대 대기 시간 도달\n");
        music(18);
    }
    pthread_mutex_unlock(&mid);
    return NULL;
}

void* first_take(void* arg) {
    while (1) {
        time_t now = time(NULL);
        struct tm* current_time = localtime(&now);
        pthread_mutex_lock(&mid);
        if (current_time->tm_sec == first_time - 1) {
            flag = 1;
            printf("알림: 하루 첫 복용 가능\n");
        }
        if (current_time->tm_sec == first_time && flag == 1) {
            printf("알림: 하루 첫 복용 마감\n");
            music(18);
        }
        pthread_mutex_unlock(&mid);
        sleep(1);
    }
    return NULL;
}

void timer() {
    pthread_t min_thread, max_thread, first_thread;
    pthread_mutex_lock(&mid);
    if (flag == 1) {
        flag = 0;

        // 스텝모터 작동(시계방향, 8칸 => 45도)
        one_two_Phase_Rotate_Angle(45, 0);
        printf("스텝모터 동작\n");

        today_count++;
        total_count++;
        printf("복용 완료, 오늘 복용 횟수: %d\n", today_count);
        if (today_count < max_count) {
            pthread_create(&min_thread, NULL, take_min, NULL);
            pthread_create(&max_thread, NULL, take_max, NULL);

            pthread_detach(min_thread);
            pthread_detach(max_thread);
        } else {
            today_count = 0;
            pthread_create(&first_thread, NULL, first_take, NULL);
            pthread_detach(first_thread);
        }
    } else {
        printf("조건 불만족\n");
        music(18);
    }
    pthread_mutex_unlock(&mid);
}

// NFC 감지 스레드
void* nfc_task(void* arg) {
    pid_t pid;
    int status;
    char* argv[] = { "nfc-poll", NULL };
    int fd = *(int*)arg;

    while (1) {
        pthread_mutex_lock(&flag_mutex);
        if (nfc_flag == 0) {
            pthread_mutex_unlock(&flag_mutex);
            printf("NFC 감지 중...\n");
            if (posix_spawn(&pid, "/bin/nfc-poll", NULL, NULL, argv, environ) == 0) {
                if (waitpid(pid, &status, 0) >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                    pthread_mutex_lock(&flag_mutex);
                    nfc_flag = 1; // NFC 인증 성공
                    pthread_mutex_unlock(&flag_mutex);
                    printf("NFC 인증 성공\n");

                    // 타이머 실행
                    timer();
                }
            } else {
                perror("nfc-poll 실행 실패");
            }
        } else {
            pthread_mutex_unlock(&flag_mutex);
        }
        sleep(1); // NFC 감지 주기
    }
    return NULL;
}

int main() {
    int fd_serial;

    if (wiringPiSetupGpio() < 0) return 1;
    if ((fd_serial = serialOpen(UART2_DEV, BAUD_RATE)) < 0) {
        printf("UART 초기화 실패\n");
        return 1;
    }

    pthread_mutex_init(&flag_mutex, NULL);
    pthread_mutex_init(&mid, NULL);

    // GPIO 설정
    for (int i = 0; i < 4; i++) {
        pinMode(pin_arr[i], OUTPUT);
    }

    pthread_t nfc_thread;

    // NFC 처리 스레드
    pthread_create(&nfc_thread, NULL, nfc_task, &fd_serial);

    pthread_join(nfc_thread, NULL);

    pthread_mutex_destroy(&flag_mutex);
    pthread_mutex_destroy(&mid);
    serialClose(fd_serial);
    return 0;
}
